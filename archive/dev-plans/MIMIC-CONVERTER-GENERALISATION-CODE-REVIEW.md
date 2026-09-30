# Converter Generalisation: Post-Implementation Code Review

**Status (2026-09-28): review complete and implemented.** The findings below describe the branch at `cb2e0ffc`; every `file:line` is relative to that commit and has moved since. The commit that follows on the same branch (its message cites this document) implemented all thirteen P2 findings, the P3 list, the simplification pass and the documentation items, and closed the PM register. Delivered beyond the findings: `inspect` now exits non-zero when a source file's inspection records an error; the generic manifest records `pool_size`/`chunksize` under a `tuning` key outside its configuration digest; the v3 battery gained a twentieth check, `position-bounds`; the consumer metadata fragment's `format_table_fields` carry `provides_core_role`; the L-Halo validation budget carries a measured base term; the forests-HDF5 adapter reads storage-ordered files through budgeted read windows (29 % faster on real micro-Uchuu, output byte-identical); the acceptance harness's ASCII extractor streams in `block_rows` slices. Two guarantees are narrower than the findings asked: the ASCII batch-term refusal (F1) fires as soon as scatter has counted every snapshot, the earliest point in the current flow at which the largest snapshot's row count exists (a separate read-only pre-count would be a further change), so the scatter pass is spent before an over-budget snapshot is refused and its output is kept for a resume under a larger budget; and the generic manifest now carries a `tuning` record, so a version 3 manifest written before this change is refused by name rather than resumed (the format has never shipped, and the acceptance workdirs it affects are disposable evidence). One error text changed during the pipeline refactor: `build_adapter`'s unknown-format message now lists the supported formats. Deliberately not done, each for a stated reason: the `rank_sort.py` split (the keyed sorter reuses the rank core's spill and run machinery, so a split would invert the dependency and break the core's self-containment test; it stays as one engine with two key layouts), the `report.py` split (643 lines, one seam, not worth a module), collapsing `TransposeBudget` (its per-phase fields are what the phase-split test checks), sharing the row spool with the keyed run files (different naming, accounting and error contracts), extracting `resolve_selection`'s typing loop (a seven-argument helper mutating two shared dictionaries would not be simpler), generating the CLI argument groups from the option tables (the tables are per format, the groups per flag), and collapsing the four `simulation_info.yaml` reads (each serves a different contract, and deriving the reference particle mass from the converted value does not round-trip bit-exactly in about ten per cent of doubles). The inspection link-scan's out-of-forest and bad-snapshot errors still abort the whole report rather than being recorded per file, and history wording that predates the branch was left in place. The implementation was itself reviewed before commit by an independent external panel (codex `gpt-6-sol` at medium effort and opencode `opencode-go/glm-5.3-flash` at max effort, both read-only, through the orchestrator); both returned PASS WITH RISKS with no P0/P1, and every finding they raised was verified and either fixed in the same commit (unvalidated completed-stage results, the pre-tuning manifest refusal, the extractor's tree-identity check, a stray log file, an unread writer measurement, two stale docstrings and one stale README sentence) or recorded above.

**Purpose:** Independent, whole-branch code review of the completed `MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md` work (Slices 1–12) on branch `converter-generalisation-mode-b`, judged against the plan's goals, code correctness and code quality, with a secondary simplification pass over the end state. It also assesses every issue the PM recorded in its run notes (`.pm/runs/<run>/notes.md`, both runs) and per-slice assessments.

**Date:** 2026-09-28. **Reviewed range:** `f5cc6004` (merge base with `main`) → `cb2e0ffc` (HEAD), 43 commits, 128 files, +41,275 / −127 lines. **Reviewer:** Claude Fable 5.1 at high effort, coordinating ten parallel review forks (one per slice group), one differential `code-health` measurement and one mechanical-gates run. Every finding below carries a `file:line` its author verified in the current tree; nothing was edited.

**Authorization status:** every slice was drift-audited and code-reviewed by the Mode B PM's independent panel during the run (HANDOFF.md). This review does not re-audit authorization; it is a quality review of the end state, in which the delivered result outranks pre-implementation plan decisions.

---

## 1. Executive summary

**Verdict: PASS WITH RISKS. No P0 or P1 findings. The plan's goals are achieved as stated, with one goal partial by design.** The converter is correct on every path the review traced, its scientific preservation claims hold up under independent re-derivation (cast order, link semantics, bit-exact float comparison, gap conservation), the legacy v2 route is verified byte-identical by diff and by test, and the acceptance evidence on real data is honest about what was real, sampled, fixture-only or unavailable. All 1,486 converter tests, `check-format`, `check-docs`, `check-generated`, `validate-modules` and `check-horizontal-fixture` pass on this host, exit 0 throughout (§5).

What remains is a **tail of 13 P2 findings** (none blocking a merge, several worth fixing before the runtime follow-on starts) and a **cleanup debt** that is the natural residue of twelve slice-scoped surfaces: helpers imported privately across modules, one new import cycle, two 3k-line files carrying two generations of code each, and a handful of dead public names. The PM's own recorded carry-forward list was accurate: **of 37 items it recorded, 21 are fixed or mitigated in the code, 15 are still open exactly as recorded, and 1 (the O(chunks²) manifest cost) can be closed as measured-negligible.** Two new documentation-of-record defects were not on the PM's list: the v3 specification still declares itself a non-normative draft that "no producer may emit", and the frozen plan's C3 contradicts the shipped spec on what `links_adjacent` asserts.

**Top actions, in order:**

1. Fix the ASCII batch-budget term (`ctrees_ascii.py:445-471`): it charges the full `--ingest-max-rows` regardless of snapshot size, so a legal `--memory-budget-mb` below ≈700 MB refuses tiny conversions, and it refuses only *after* the multi-minute ASCII preparation has run and mutated the workdir. Reproduced by two reviewers on the CLI. (§3 F1)
2. Hoist the `simulation_info` SHA-256 check in `hdf5_writer._check_configuration` out of the per-route `elif` chain so the forests-HDF5 route is bound too; today a direct `pipeline.run_write` caller can stamp Uchuu data with micro-Uchuu's box size. (§3 F7)
3. Shape-check `sources.inventory` when a manifest is loaded, so a hand-edited manifest yields a named FAIL from `validate`/`report` rather than a raw `KeyError` traceback. (§3 F6)
4. Correct the v3 spec's status banner and line 433 (Gate G1 was approved 2026-09-23; the CLI emits v3 by default), and record the C3 `links_adjacent` scope correction as the post-run plan amendment the plan itself invites. (§3 F11, F12)
5. Budget the adapter emission buffer on the L-Halo and HDF5 routes the way the ASCII route already does, and stream the acceptance harness's ASCII extractor, before anyone runs either at full-Uchuu scale. (§3 F2, F9)
6. Schedule one deliberate simplification pass (§6): promote the cross-adapter private helpers into a shared module, break the `column_schema ↔ ctrees_parser` cycle, split the v3 halves of `validate.py` and `rank_sort.py` into their own files, and delete the dead names.

---

## 2. Plan goals: achieved?

| # | Goal (plan §"Goals and completion boundary") | Evidence | Status |
|---|---|---|---|
| 1 | Conversion paths for all five named simulations using their shipped formats | README route table; acceptance §2: mini-Millennium, micro-Uchuu (three formats), Millennium 0–15, mini-Uchuu 0–15 converted on real data; full Uchuu on its committed fixture only | **Achieved** (full Uchuu is a route, not a conversion, by the plan's own definition) |
| 2 | Declarative extra fields with native precision, units and halo association | Six mini-Millennium and three forests-HDF5 extras bit-equal to independent extraction over every row (1.5 M and 22.6 M rows) | **Achieved** |
| 3 | Preserve source topology, chain order, forest enumeration, row identity, values; ctrees conventions only for ctrees | 25/25 `compare` checks on every route against the unmodified C vertical reader; 0 unit-ordinal mismatches; cast order verified line by line against `read_ctrees_hdf5.c` and `read_ctrees_ascii.c` (§3 verified-correct list) | **Achieved** |
| 4 | Skipped-snapshot links and populations above int32 | 29,291 gaps conserved, max span 2, independent raw-h5py census; int64 end to end with no narrowing found; >2³¹ rows and >2⁵³ keys exercised **synthetically only** | **Partial by design**: no real wide dataset exists; the plan foresaw this and the docs say so |
| 5 | Preserve the ASCII→v2 workflow, valid v2 datasets, v1 rejection | v2 modules' diffs are docstring/import-only plus a refuse-before-emit guard; fresh v2 run: totals exact, 15/15 battery, 8/8 crosscheck, no phase >20 % slower; v1 rejection untouched | **Achieved** |
| 6 | Reproducible provenance, safe restart, bounded processing, independent validation | Manifest v3 with embedded schema/digest/inventory; interruption-resume tests; per-term budgets measured just under the 2 GiB ceiling; C dump and comparator share no converter code (pinned by test) | **Achieved** ("bounded" means per-term buffers, not RSS, as C4 defines and the docs state) |

The completion boundary is respected everywhere: every stage prints `runtime support: NONE`, every report opens `NOT RUNNABLE BY THE CURRENT MIMIC`, no changed public document equates conversion with runtime support, and the C reader's v3 rejection is triple-guarded (`read_horizontal_hdf5.c:282,289,1066`).

---

## 3. Findings

Severity follows the code-review skill: P0/P1 require a stated reachability path; unreachable-today defects cap at P2. **There are no P0 or P1 findings.** Findings are ordered by impact, not by slice.

### P2 — should be fixed soon

**F1. `scripts/convert/adapters/ctrees_ascii.py:445-471` — ASCII batch-budget term charges `max_rows` unconditionally and fires after preparation.** `batch_bytes = 2 × max_rows × (fixed_itemsize + 36 + 152 + extras)` is computed once and added to every per-snapshot check, so at the CLI default `--ingest-max-rows 1048576` it is ≈664 MiB regardless of snapshot size. Reproduced on the 4-halo committed fixture: `convert_trees.py ingest --source-format consistent_trees_ascii --memory-budget-mb 512` exits 1 with "snapshot 48 (1 halos …) needs 698,875,952 bytes". Worse than the PM record states: the refusal is raised inside `iter_batches`, after scatter/sort/fixups/links have completed and the workdir has been mutated, though every input to the check was known at `pipeline.initialize`. `test_cli.py:336-341` works around it with `--ingest-max-rows 4096` rather than pinning it. *Fix:* charge `min(max_rows, n_rows)` inside the loop, and pre-check the batch term in `initialize()` where `transpose.plan_budget` already runs; replace the test workaround with a regression test. (Groups 4, 8; PM Slice 5/9 carry-forward, still open.)

**F2. `scripts/convert/adapters/lhalo_binary.py:618-688` (mirrored in `ctrees_hdf5.py`) — the adapter emission buffer is the one scaling term never compared against `memory_budget_bytes`.** Per batch the adapter holds `max_rows × 104 B` raw plus ≈140 B/row canonical columns plus builder parts, doubled at concatenation (`:1240`): ≈400–500 MB at the default `max_rows`, ≈4–5 GB at `--ingest-max-rows 10000000`, with no refusal. The ASCII adapter charges exactly this term (F1), so the three adapters disagree on what C4 bounds. *Fix:* add one `_check_budget` call for `2 × max_rows × (itemsize + row width + extras)` before the loop in both prelinked adapters. (Group 2.)

**F3. `scripts/convert/adapters/source_inventory.py:645-669` — HDF5 inspection materialises three whole-file int64 arrays per `FileN` group (≈24 B/halo).** For a full-Uchuu file (≈90 M halos) that is ≈2.2 GB per group, with no budget flag. Inspection is outside C4's converter scope, but `inspect` is the tool the docs say to run first on a new source. *Fix:* derive target snapshots per chunk by bisecting the sorted offsets instead of `np.repeat`, or document the O(halos-per-file) cost in `inspect --help`. (Group 2.)

**F4. `scripts/convert/adapters/ctrees_hdf5.py:1197-1246, 1290-1300` — per-forest hyperslab reads, twice over.** `_read_forest_topology` reads six columns per forest, then `_columns` re-reads all five links, twelve float roles, `id` and snap per forest chunk: >10 M tiny h5py reads for real micro-Uchuu (440,651 forests, mean 51 halos). Slice 11 measured 549.9 s versus 502.7 s for the L-Halo route on identical data; at full-Uchuu scale (≈10⁹ forests) the per-call overhead dominates. Correctness is unaffected. *Fix:* since `validate_forest_table` already proves forests tile the datasets, read `[offset, offset+window)` windows across consecutive forests when `ForestInfo` is in storage order (it is on every real source; the test at `:1706` asserts it), falling back to per-forest reads otherwise. (Group 3.)

**F5. `scripts/convert/pipeline.py:716-718, 760-800` — ingest resume costs ≈1.9× the completed work at scale, and nothing documents it.** On resume every registered chunk is re-hashed from disk *and* the adapter is re-read from the source start with every batch re-serialised and hashed for comparison. This is the sanctioned "prove the source still yields the same rows" design and is correct, but at ≈25 TB of chunks a crash at 90 % of ingest costs two full passes plus a source re-read. *Fix:* document it in the README restart section; optionally offer an explicit opt-in cheaper mode trusting the chunk SHA prefix (still verify-on-disk), since the inventory record and per-chunk `first/last_source_halo_id` already pin the enumeration. (Group 6.)

**F6. `scripts/convert/validate.py:3317` (also `:2990-2991`, `:3101`) and `conversion_manifest.py:433-438, 608-635` — manifest inventory and stage records are indexed without shape checks and escape the named-error boundary.** `ConversionManifest.load` verifies a digest over `configuration` only; `sources.inventory` is unprotected, and the battery does `inventory["selected_halos"]`, `inventory["total_halos"]`, `inventory["files"]`, `entry["n_units"]` inside a `try` that catches only `RankSortError`. `_read_json` catches only `ValueError`, so a directory or unreadable file named `manifest.json` raises a raw `OSError` from `classify_manifest`. Reachable via `convert_trees.py validate|report` on any workdir whose manifest was hand-edited or partially corrupted. Fails loud, never wrong. *Fix:* shape-check `sources.inventory` and `stages[name]` in `load()` the way top-level keys already are, and catch `OSError` in `_read_json`. (Groups 6, 7; PM Slice 7/8 carry-forward, still open.)

**F7. `scripts/convert/hdf5_writer.py:917-946` — `_check_configuration` binds `simulation_info` on two routes but not the forests-HDF5 route.** The pipeline records `simulation_info` optionally for both L-Halo and forests-HDF5 (`pipeline.py:325, 350`) and the CLI always supplies it, yet only the `lhalo_binary` branch (`:936`) checks the recorded SHA-256; the `consistent_trees_hdf5` branch checks `particle_mass` only. Uchuu and micro-Uchuu share `particle_mass` (0.0327) and differ only in box size, so a direct `pipeline.run_write` caller (a documented API used by tests) would stamp the wrong box size and cosmology. The CLI's `_require_recorded_simulation_info` (`convert_trees.py:551`) closes this for CLI users only, as the PM disclosed. *Fix:* hoist the `"simulation_info" in parameters` SHA-256 check out of the `elif` chain so it applies to every route; three lines, and it removes the duplicated error text. (Group 7.)

**F8. `scripts/convert/convert_trees.py:551-570`, `hdf5_writer.py:915-945` — nothing checks `--simulation-info` against the source data, and a cheap partial guard is available.** Confirmed as the PM recorded: mini-Millennium and Millennium share `tree_name`, particle mass, cosmology and 64 snapshots, differing only in `box_size` (62.5 vs 500), and nothing in ingest, write or the v3 battery reads the payload against the header. Pairing Millennium files with mini-Millennium metadata converts self-consistently and wrongly. *Fix:* a one-directional check at write or validate, `max(Pos) ≤ box_size_mpc_h` per snapshot on data the writer already streams, catches the Millennium-data/mini-metadata direction for free; the reverse direction is not detectable this way and should stay documented. (Group 8.)

**F9. `scripts/convert/tests/run_generalisation_acceptance.py:1718-1776` — the ASCII independent extractor is still unbounded in memory.** `_ascii_trees` accumulates every token of a whole `#tree` block as a list of lists (`:1735`), then `iter_ascii_source` materialises the same tree again as a `<U` array (`:1773`), so each tree is resident twice; neither `--block-rows` nor `--budget-mb` reaches this path, and the first counting pass builds full row lists merely to take `len(rows)`. Measured at 3.83 GiB on micro-Uchuu ASCII (largest forest 350,074 halos) on a 512 GiB host; untested on smaller machines. *Fix:* count rows without retaining tokens in pass one, and parse a tree in `block_rows` slices in pass two (unit prefix sums are known after pass one). (Group 9; PM Slice 10/11 carry-forward, still open.)

**F10. `tests/unit/tools/dump_ctrees_topology.c:284-286` — the `--source-payload` enumerated-reader branch has no regression test.** `CDumpToolTests` builds the ASCII tool but runs it only in default v1 mode (`test_generalisation_acceptance.py:1632-1650`); every `--source-payload` assertion uses the per-file L-Halo synthetic. Slice 11 exercised the branch empirically on real ASCII/HDF5 data, so this is a regression-protection gap in a scientific-evidence instrument, not a known defect. *Fix:* add a `--source-payload` run of the ASCII tool over `micro-uchuu-ascii/_tests/data` against a hand-typed literal dump, mirroring `test_source_payload_matches_the_literal_oracle_byte_for_byte`. (Group 9; PM Slice 10 carry-forward, still open.)

**F11. `docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md:1-12, 99-103, 433` — the v3 specification still declares itself a non-normative draft after Gate G1 cleared.** The status block reads "Status: DRAFT. Not normative. Nothing in this document is a contract yet … no later slice may treat this file as normative, no producer may emit version 3 data", and line 433 says the consumer review "is unapproved at the time of writing". Both are false: `MIMIC-V3-CONSUMER-DESIGN-REVIEW.md:3` carries `**APPROVED 23-09-2026 by Darren Croton**`, the pathway calls the spec "normative for the converter via Gate G1", and `convert_trees.py`, `hdf5_writer.py`, `validate.py` and the README all cite this file as the contract they implement. A reader who opens the spec first is told the shipped converter violates its own contract. The file was in no slice's surface after Slice 2, so nobody could fix it. *Fix:* replace the banner with "Normative for the producer since Gate G1 (2026-09-23); runtime support pending" and correct line 433. Whether to promote the content into `HORIZONTAL-HDF5-FORMAT.md` (the plan said "after the format actually ships") is an owner decision tied to runtime-plan R0-11. (Group 10; found independently by the coordinator.)

**F12. Contract defect: `MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md:130` vs `HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md:185, 206` — `links_adjacent` scope.** C3 says "`1` asserts all non-null links are adjacent"; the shipped spec, writer (`hdf5_writer.py:964`) and validator measure it over `Descendant` links only, explicitly rejecting the all-five-links reading (which would stamp 0 on ≈92 % of real data, since NextProgenitor is descendant-relative). The spec's reading is the coherent one and matches v2's precedent. The frozen plan is wrong and, being frozen, was never amended. Same bucket: C3's "NextProgenitor can target a different *earlier* snapshot than its sibling" is owner-relative wording the consumer review corrected. *Fix:* record both as the post-run plan amendment the plan's own "category 3" text invites, or a future reader of C3 will "fix" the producer. (Group 10.)

**F13. `scripts/convert/adapters/ctrees_ascii.py:473, 481` — every interior snapshot's fixed and links scratch is md5-verified twice per pass.** `_open_snapshot(snap+1)` for the upcoming `SourceHaloID` column and `_open_snapshot(snap)` on the next iteration each call `Manifest.verify_intermediate`, which recomputes `file_md5` over the whole file; only the id column is cached and the verified memmaps are discarded (`:483`). Zero correctness impact; roughly doubles checksum I/O over the largest scratch artifacts, and the transpose inherits it. *Fix:* carry the verified `(fixed, links)` memmaps in `upcoming` alongside the ids. (Group 4; PM Slice 5 carry-forward, still open.)

### P3 — worth a line each

Grouped by module. "PM" marks an item the PM had already recorded and tolerated.

*Schema and contracts (`column_schema.py`, `adapters/base.py`)*
- `column_schema.py:270, 293, 225` — `_ASCII_ROLES`, `_CTREES_HDF5_ROLES` and `_CORE_PAYLOAD_NAMES` are hand copies of `ctrees_parser`'s columns, `read_ctrees_hdf5.c:111`'s names and `PAYLOAD_FIELDS` with no mechanical pin (PM, Slice 2 → Slice 5, left open). Runtime probe shows all three equal today; drift would fail loud at parse time. Three one-line pins close it.
- `base.py:20-64`, `column_schema.py:1-13` — module docstrings carry slice narrative ("arrive in Slices 3–5", the Slice 1 bare-`Exception` anecdote). STYLE-GUIDE: history belongs in commit messages; restate the four contract bullets in present tense.

*L-Halo and inspection (`lhalo_binary.py`, `source_inventory.py`, `inspect_sources.py`)*
- `test_lhalo_adapter.py:2110-2116` — `test_file_prefix_tree_numbers_match_the_vertical_enumeration` resolves `simulation_dir` unanchored and silently skips from any cwd but repo root (PM, carried six rounds). Anchor against `REPO_ROOT` as `inspect_sources._anchor_simulation_dir` does.
- `source_inventory.py:415-460` — inside `inspect_ctrees_hdf5_source` the structural validators run outside the per-file `try/except` the docstring promises, so one malformed `FileN` aborts the whole report instead of being recorded as that file's error.
- `lhalo_binary.py:747` vs `ctrees_hdf5.py:1289` — the same validation-budget formula has a `TOPOLOGY_BASE_BYTES` addend on one route and none on the other; probably deliberate, undocumented.

*forests-HDF5 (`ctrees_hdf5.py`)*
- `:131-158` — the "Deliberate differences from the C reader" docstring still omits the payload non-finite/float32-overflow rejection (`:366-389, 436-443, 1360-1368`) that three Slice 4 reviewers flagged (PM, carried to Slice 12, whose surface excluded this file).
- `:151-153` — per-file snapshot-spelling resolution is listed as a *tightening* but is a *loosening*: the C reader detects the field once on the first file and reuses the name; a dataset mixing `Snap_num`/`Snap_idx` fails in C and converts here. Unreachable on shipped data; the docstring should not call it a tightening.

*ASCII path (`ctrees_parser.py`, `scatter.py`)*
- `scatter.py:416-420` — `Manifest.register_intermediate` docstring still says scratch always carries the frozen `DTYPE_TAG`; false since Slice 5 (PM).
- `ctrees_parser.py:975-999` — NA-token diagnostics report `nan` rather than the source token because pandas converts `NA`/`N/A` before the integer parser sees it; message is still understandable. Note only.

*Transpose (`transpose.py`, `source_keys.py`, `rank_sort.py`)*
- `rank_sort.py:1633` — `sorted_blocks` budget/consumer args are bare-`int()` cast and the merge-floor checks fire on first `next()`, not at call time as the docstring invites; every current caller passes ints and primes safely (PM, unreachable).
- `source_keys.py:219` — `SnapshotLayout` counts coerced by bare `int()` while `validate_snapshots` strictly type-checks; both callers pass integer arrays (PM, unreachable).
- `transpose.py:167, 173` — per-row scratch constants understate the measured transient peak (≈25 B/row vs declared 18 in `_rows_of`; `batch.validate()`'s 9 B/row uncounted): a 2–3 % under-report of `peak_resident_bytes` that the 1 MiB tracemalloc allowance cannot see. Same class as the Slice 3 lesson.
- `rank_sort.py:1477` — docstring over-promises "run order, then input order within a run" for equal keys; harmless for every current (total) key.

*Manifests and pipeline (`conversion_manifest.py`, `pipeline.py`)*
- `conversion_manifest.py:681` — `save()` opens `manifest.json.tmp` with plain `open(tmp, "w")`, following a pre-planted symlink; every other write path is symlink-hostile (PM). Use `O_NOFOLLOW`.
- `conversion_manifest.py:366-367` — `merge_dependencies` never compares two non-null SHA-256s for the same path (PM, unreachable; one `elif` fixes it).
- `pipeline.py:1104-1117` — `read_transposed` is exported with no production caller; the writer reads transposed files itself.
- `pipeline.py:363-372` — `--pool-size`/`--chunksize` are frozen into the configuration digest, so a full-configuration resume with a different worker count is refused as "a different conversion" although neither affects output; `--workdir`-only resume works.

*Writer, validator, report (`hdf5_writer.py`, `validate.py`, `report.py`)*
- `validate.py:1798` — comment says the budget shares "sum to less than one"; 6+4+2+1+3 = 16 = the denominator (PM).
- `hdf5_writer.py:642` — `_V3_PINNED_PAYLOAD_TYPES` is dead (one reference, its definition); the validator has its own used table (PM).
- `hdf5_writer.py:964`, `validate.py:2585` vs `source_keys.py:632` — three predicates for "gapped descendant" (`span > 1` vs `span != 1`); agree on conforming data.
- `test_hdf5_writer.py:1096-1155` — write verification has no negative test for a `/schema` attribute mutation, a sidecar row mutation, or the zero-halo file's chunk shape (PM). Code paths read correctly.
- Sampled inventories get count/range/uniqueness coverage only (PM, by design, unreachable: no route exposes a sampled inventory).

*CLI (`convert_trees.py`)*
- `convert_trees.py:192-228` plus `pipeline.py:604-612` — `simulation_info.yaml` is read four times before it is pinned (PM recorded three); edit-during-setup race fails loud at write. Read bytes once and hash them.
- `write --format-version {3}` is a one-choice option parsed but never read (PM).
- `inspect` prints the runtime notice both as a JSON field and as a stdout trailer, breaking a piped `jq` consumer (PM).
- `test_cli.py:596-628` — in-process writer test leaks a `write:` line to the runner's stdout.

*Comparator and C tool (`run_generalisation_acceptance.py`, `dump_ctrees_topology.c`)*
- `:1460-1546` — a profile with a duplicate extra name or one named `SourceHaloID` reaches `np.dtype` and raises a bare `ValueError` (exit 1, no record) instead of the documented exit-2 contract (PM).
- `:2331`, `:994-999`, `:741-760` — negative `--block-rows` is not rejected: `V3Dataset` yields nothing (misleading `nothing_compared` FAIL) and `SourceDump.blocks` buffers the whole dump (PM).
- `:2177-2181` — log stems are second-resolution; same-label retries in one second overwrite each other while both record entries name the same path (PM).
- `:2390-2397` — `compare-extras` records only `--info-file` as an input on the forests-HDF5 route, omitting the ExternalLink target that supplied every value (PM).
- `:2192-2200` — `measured_run` blocks in `os.wait4` with no timeout and no `KeyboardInterrupt` handling; an interrupted harness leaves the child running.
- `:931` — snapshot files sorted lexicographically while the number is parsed from the stem; correct below 1000 zero-padded snapshots. Sort by the parsed number.
- `dump_ctrees_topology.c:245, 273-277` — partial output left on disk after a missing-file `FATAL_ERROR` (PM, tolerated; the `# end` trailer makes truncation detectable).

*Documentation*
- `scripts/convert/README.md:36-40` — the full-Uchuu route row pairs production flags (`--last-file 1999`, "2000 / 0 present") with fixture-only evidence; copy-pasting it fails, and the evidence-bearing fixture invocation lives only in the acceptance doc §7 (PM addendum).
- `scripts/convert/README.md:40` — the quoted missing-file error is the adapter's (`lhalo_binary.py:500`), but the CLI route emits `conversion_manifest.py:319`'s "source dependency … cannot be pinned" first, as the acceptance doc §2.3 records; a user grepping for the documented message will not find what they saw.
- `docs/USER-GUIDE.md:364` — stale singular "the shipped horizontal package" (PM).
- `MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md:100, 424-429` — R0-7(b) lacks the "requires amendment" table mark; Slice 7's four packages don't tie link-role declarations to R0-2 (PM).
- `MIMIC-DEVELOPMENT-PATHWAY.md:172, 178, 191, 202, 225` — "Slice 12 delivered for review" is now stale; the run completed. The implementation plan's own `Status: … No implementation has started` (line 5) is likewise stale by design and needs an "executed" banner as a post-run amendment.
- `MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md:206-215` — the two supplementary scripts behind the headline gap census (`gap_census.py`, `unit_ordinals.py`) exist only under gitignored `/Volumes/Scratch/mimic-slice11/`. STYLE-GUIDE's rule is met in letter (facts stated inline, method described "closely enough to rewrite"), but the plan's headline acceptance figure rests on a 30-line uncommitted script. Commit both under `scripts/convert/tests/tools/`.
- Prior PM note: `scripts/convert/README.md` now does document `--source-payload` and `TOPOLOGY_DUMP_BUILD_DIR` (closed).

### Contract notes (not rated against the implementer)

- **Sample ranges not starting at file 0.** C1 says "for sampled trees, retain the parent inventory's prefix counts … do not compact sample identities". The implementation reads a sample as a *leading* range: `SourceInventory` prefix-sums over the requested files only, the C dump starts its per-file offset at the first requested file, and `README.md:44` states "a sample must be a range starting at file 0 if its identities are to match an unsampled conversion's". Both sides agree with each other, Slice 11 used only leading ranges, and the limitation is documented, so this is a narrowed-but-coherent reading rather than a defect. It would be cheaper to make the tool refuse `--first-file > 0` unless a `--allow-compacted-sample` flag is passed than to leave the trap to the README.
- The plan C4 "no writer" non-goals on Slices 6 and 7 versus the transpose's spool output and the `StageWriter` interface were settled by the PM on the record; the code matches the settled reading.

---

## 4. The PM's recorded issues: status in the end state

The PM's notes (both runs) and per-slice assessments recorded 37 distinct carry-forward items. Each was re-checked against the current tree.

| # | PM item (slice) | Status now | Evidence |
|---|---|---|---|
| 1 | NextProgenitor is descendant-relative; never assume owner-adjacent (S2) | **Honoured** | `source_keys.py:603`; consumer review; spec `:185` |
| 2 | `links_adjacent` is Descendant-scoped only (S2) | **Honoured** in code and spec; **plan C3 still says otherwise** | F12 |
| 3 | SourceHaloID never confused with MostBoundID (S2) | **Honoured** | all remap keys are SourceHaloID; comparator uses MostBoundID as data only |
| 4 | Filesystem identity via `(st_dev, st_ino)` (S3) | **Fixed** | `lhalo_binary.py:509-527`; `source_inventory.py:824-830` |
| 5 | Explicit type-check on every public scalar via shared helper (S3) | **Fixed** | `_require_integer` at `lhalo_binary.py:376, 387, 470, 628`; but imported privately by two other modules (§6) |
| 6 | OSError wrapping after `open()`; probe retry (S3) | **Fixed** | `lhalo_binary.py:303-308, 577-583, 699-714` |
| 7 | Budget check strictly before allocation (S3/S4 ruling) | **Verified on every route** | `lhalo_binary.py:564→574, 747→754`; `ctrees_hdf5.py:1204-1215, 1029-1036` (test patches `np.empty`) |
| 8 | Per-term budget, not summed, not RSS (settled ruling) | **As designed**; but the *emission buffer* is unbudgeted on two routes | F2 |
| 9 | `CanonicalBatch.validate()` does not verify IDs vs inventory prefix sum (S2) | **Mitigated downstream** | all adapters use one `SourceInventory.base_id`; battery checks `{1..N}` coverage `validate.py:2982-3029`; comparator re-derives |
| 10 | `_ASCII_ROLES`/`_CORE_PAYLOAD_NAMES` hand copies unpinned (S2→S5) | **Open** | §3 P3; runtime-equal today |
| 11 | cwd-skip of the vertical-enumeration test (S3) | **Open** | `test_lhalo_adapter.py:2110-2116` |
| 12 | `ctrees_hdf5.py` docstring omits non-finite rejection (S4→S12) | **Open** | `:131-158` |
| 13 | `max_snapshot=len(a_list)-1` caller obligation (S4→S9) | **Closed** | `pipeline.py:472, 607, 713`; `convert_trees.py:403` |
| 14 | Resume must check source content, not path (S5) | **Fixed** | `ctrees_ascii.py:277` + test |
| 15 | New sidecar must be in `source_intermediates` (S5) | **Fixed** | `scatter.py:401-402` + test |
| 16 | `batch_bytes` charges `max_rows` unconditionally (S5→S9) | **Open, reproduced, worse than recorded** | F1 |
| 17 | Double md5 per interior snapshot (S5) | **Open** | F13 |
| 18 | `register_intermediate` docstring stale (S5) | **Open** | `scatter.py:416-420` |
| 19 | ASCII batch order snapshot-major; transpose must re-sort (S5/S6) | **Honoured** | transpose re-sorts by `(SnapNum, SourceHaloID)` |
| 20 | `SnapshotLayout` bare `int()` counts (S6) | **Open, unreachable** | `source_keys.py:219` |
| 21 | `sorted_blocks` deferred budget check / bare `int()` (S6) | **Open, unreachable**; all callers prime | `rank_sort.py:1633` |
| 22 | O(chunks²) manifest registration/save (S7) | **Close as measured**: ≈173 k chunks at full Uchuu ⇒ ≈12 min CPU, ≈150 GB cumulative fsynced JSON over a 25 TB conversion | Group 6 probe |
| 23 | `save()` `.tmp` without `O_NOFOLLOW` (S7) | **Open** | `conversion_manifest.py:681` |
| 24 | `merge_dependencies` two non-null SHAs (S7) | **Open, unreachable** | `:366-367` |
| 25 | `load()`/`_read_json` shape and OSError gaps (S7) | **Open, reachable via hand-edited manifest** | F6 |
| 26 | Skip-trust `allow_removed` vs transitive verify (S7) | **Confirmed no data-integrity gap** | `run_write` strict re-verify `pipeline.py:1239` |
| 27 | `consume_stage` three idempotency cases (S7) | **All present and correct** | `pipeline.py:898-930` |
| 28 | Sampled-inventory coverage gap (S8) | **Open by design, unreachable** | `validate.py:2991` |
| 29 | `_V3_SHARES` comment arithmetic (S8) | **Open** | `validate.py:1798` |
| 30 | KeyError on manifest missing `inventory.files` (S8) | **Open** | F6 |
| 31 | `_V3_PINNED_PAYLOAD_TYPES` unenforced (S8) | **Open, dead** | `hdf5_writer.py:642` |
| 32 | No negative tests for `/schema` and sidecar corruption (S8) | **Open** | `test_hdf5_writer.py:1096-1155` |
| 33 | Writer L-Halo metadata branch (S9 grant) | **Present and tested** | `hdf5_writer.py:936`; `test_cli.py:590-628` |
| 34 | forests-HDF5 route particle_mass-only under direct `run_write` (S9) | **Open** | F7 |
| 35 | `--simulation-info` vs source data unchecked (S9) | **Open; cheap partial guard available** | F8 |
| 36 | `simulation_info` read thrice race (S9) | **Confirmed (four reads), low** | §3 P3 |
| 37 | Comparator items: ASCII extractor memory; enumerated branch untested; external-link targets; log collision; negative `--block-rows`; malformed profile bare exception; partial dump file (S10/S11) | **All open** as recorded | F9, F10, §3 P3 |
| — | Slice 12 cosmetics: R0-7(b) mark; USER-GUIDE.md:364; Slice 7 packages vs R0-2; README full-Uchuu row; "~8.4 TB free" will drift | **All still present** | §3 P3 |
| — | Slice 11 supplementary scripts unrecorded | **Still uncommitted** | §3 P3 |
| — | Full-Uchuu production validation unperformed | **Correctly disclosed everywhere** | acceptance §1, §2.4 |

Two items the PM did **not** record: F11 (stale spec banner) and F12 (plan C3 vs spec). Both arose because the affected files fell outside every later slice's surface, which is the expected failure mode of strict surface control and the reason a post-run review exists.

---

## 5. Validation run for this review

| Gate | Result |
|---|---|
| `git status --short` before and after | clean before; after, only this untracked review document |
| `git diff --check f5cc6004..HEAD` | exit 0, no whitespace errors |
| `make check-format` | exit 0: 193 files unchanged, 10 skipped |
| `make check-docs` | exit 0: internal links/anchors resolve, no unresolved markers |
| `make check-generated` | exit 0: all six sub-checks pass, generated code up to date |
| `make validate-modules` | exit 0: 65 properties, 20 modules, dependencies and parameter units OK |
| `make tests-converter` (full converter unittest suite, verbose) | **exit 0: Ran 1,486 tests in 255.99 s, OK; 0 failures, 0 errors, 0 skipped** (log `archive/test-logs/gate6_tests-converter.log`; `FAIL —`/`SKIP —` lines near its end are validator stdout from a deliberately broken fixture inside a passing self-test) |
| `make check-horizontal-fixture` | exit 0: `conformance: PASS` on the micro-uchuu-horizontal fixture |
| Targeted test modules run by the review groups | `test_column_schema` 121, `test_adapter_contract` 48, `test_lhalo_adapter` 113 (real-data cases executed), `test_inspect_sources` 71, `test_ctrees_hdf5_adapter` 84, `test_parser` 75, `test_scatter` 111, `test_sort_index` 25, `test_fixups` 68, `test_links` 94, `test_ascii_adapter` 25, `test_transpose` 26, `test_source_keys` 34, `test_rank_sort` 62, `test_conversion_manifest` 43, `test_pipeline` 38, `test_hdf5_writer` 68, `test_validate` 149, `test_report` 10, `test_cli` 40, `test_generalisation` 8, `test_generalisation_acceptance` 49: **all OK, exit 0** |
| C dump tool under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` | **no diagnostics**; default generated state restored, tree clean |
| `ruff check --select F401,F841,F811,F821 scripts/convert` | **clean** (no unused imports/variables, no undefined names) |
| Differential `code-health` (base `f5cc6004`) | measured; interpreted in §6 |
| Lint skill (differential) | not re-run; run per slice by the PM. Coverage stated, not assumed |

All gate logs are under `archive/test-logs/gate*.log` (gitignored). The gates were run sequentially, never concurrently with each other; the review groups' targeted modules ran alongside, which affects wall time only.

---

## 6. Simplification pass (code-simplifier lens, recommendations only)

The end state trumps the plan's slice boundaries. Twelve strictly scoped surfaces produced correct code with predictable seams; these are the places where a deliberate cleanup pass would pay for itself. None changes behaviour; each names the check that proves it.

**Measured context (differential code-health, base `f5cc6004`):** 22 production files (+20,655 code lines), 27 test files (+25,540), tests-to-code ratio 1.24. Complexity tails are concentrated in the new v3 verify/battery functions: `hdf5_writer._verify_v3_snapshot_file` CC 34, `validate.run_battery_v3` 28, `validate._v3_check_headers` 27, `validate._v3_snapshot_structure` 26, `run_generalisation_acceptance.compare_extras` 25, `dump_ctrees_topology.c:main` 25. `validate.py` is now 2,927 code lines (+1,521). One **new import cycle**: `column_schema.py ↔ ctrees_parser.py`. `ctrees_parser.py` (fan-in 30) and `column_schema.py` (fan-in 29) are the hubs; `pipeline.py` is the widest importer (fan-out 11). Duplicate windows of 12–14 lines exist between the two prelinked adapters' docstrings and budget helpers.

**S1. Promote the cross-adapter private helpers into a shared module.** `ctrees_hdf5.py:169-173` imports `_BatchBuilder`, `_require_integer`, `_validate_tree`, `VALIDATION_BYTES_PER_HALO` from `lhalo_binary`; `ctrees_ascii.py:108` imports `_require_integer`; `pipeline.py:251` reimplements it as `_strict_int`; `ctrees_hdf5.py:874-881` duplicates `_check_budget` verbatim from `lhalo_binary.py:607-613`. The comment at `ctrees_hdf5.py:160-170` admits this was a slice-surface workaround. Move `_require_integer` (absorbing `_strict_int`'s `minimum`) into `adapters/base.py`, and `_validate_tree` + `_walk_chain` + `_BatchBuilder` + `_check_budget` into `adapters/topology.py` or `adapters/_shared.py`. This also forces the F2 and `TOPOLOGY_BASE_BYTES` decisions to be made once. *Proof:* the three adapter test modules plus `test_pipeline`.

**S2. Break the `column_schema ↔ ctrees_parser` cycle.** Move `ConverterError` to a leaf module (`errors.py`); then `ctrees_parser._EXTRA_STORAGE` (a hand copy of `column_schema.EXTRA_TYPES`) and the `_EXTRA_STORAGE[t][0][1] == "i"` string-poke used at `ctrees_parser.py:366, 490` can become `EXTRA_TYPES[t].is_integer`. Also pins item 10 in §4 mechanically. *Proof:* `test_parser`, `test_column_schema`, import-order smoke.

**S3. Split the two-generation files.** `validate.py` is two batteries with a clean seam at `:1660`; the v3 half (1.7k lines) can move to `validate_v3.py` importing `Outcome`, `_examples`, `_filter_failures`, `check_header_bounds`, `RUN_SCOPED_ATTRS`. Likewise the v3 section of `hdf5_writer.py` (`:597+`) and `report.py`; and `KeyedSorter`, `SpillLedger`, `read_into`, `ResidencyMeter` and the `keyed_*_bytes_per_record` helpers out of `rank_sort.py` (now hosting two external-sort engines) into `keyed_sort.py`. File splits, not a framework. *Proof:* `test_validate`, `test_hdf5_writer`, `test_report`, `test_rank_sort`, `test_transpose`; `crosscheck.py` imports from `validate` must keep resolving.

**S4. One stage runner in `pipeline.py`.** `run_ingest`/`run_transpose`/`run_write` each repeat open-for-stage → skip-if-complete (verify, optional consume, return) → `begin_attempt` → `try … complete_stage except BaseException: fail_stage; raise` → optional consume. A `_stage_attempt(manifest, stage, directory)` context manager plus `_skip_if_complete` removes ≈40 duplicated lines and puts the "fail never completes" invariant in one place. Finish `_Route`: `build_adapter` (`:446-483`) and `_adapter_source_paths` (`:486-494`) are `if source_format ==` chains that belong as two more callables on the route table so a new format touches one place. *Proof:* `test_pipeline` (38 incl. interruption cases), `test_cli`.

**S5. `report` re-runs the full battery.** `run_report_v3` re-executes every check, so `validate` then `report` walks every dataset twice (minutes at mini-Uchuu 0–15 scale). Accept a `V3BatteryResult` or a saved outcomes file. *Proof:* `test_report`, `test_cli` output-identification tests.

**S6. Windowed reads in `ctrees_hdf5.py`** (F4) also let `_read_forest_topology` and `_columns` share one read per column instead of two; and the identity/coordinate construction at `:1273-1288` mirrors `lhalo_binary` line for line (an `identity_columns(...)` helper in S1's module).

**S7. Delete or justify dead names.** `hdf5_writer._V3_PINNED_PAYLOAD_TYPES`; `ColumnMap.aliases_for` (`column_schema.py:745`); `SourceInventory.source_halo_id()` (`base.py:224`; `coordinate()` is defensible as the invertibility proof); `pipeline.read_transposed` (test-only); `write --format-version {3}`; `_validate_progenitors`'s dead `snapshot` parameter (`lhalo_binary.py:1043-1047`); the five link entries in `column_schema._CORE_ROLE_BINDINGS` (`:1338`) that `consumer_metadata_fragment` never looks up (either emit them under `format_table_fields`, which would make the fragment more useful, or drop them and narrow the test); `_declare_checks` called twice per `compare` (`run_generalisation_acceptance.py:1292, 1420`). Duplicate constants: `INT64_MAX` vs `_MAX_RECORD_EXTENT`; `fixups.EXTENDED_FIXED_DTYPE_TAG_PREFIX` built by `.replace("scratch","fixed")` (write the literal); `ctrees_ascii.py:774` builds `"extra_" + name` literally instead of `EXTRA_FIELD_PREFIX`.

**S8. Smaller local items.** `TransposeBudget` carries 17 fields most equal to `half` or `usable − half`; collapse to the four distinct shares. `_resolve_chunk` has eight parameters and returns a cursor tuple; a small `_LinkCursor` removes the threading. `_RowSpool` and `_KeyedRunWriter/Reader` duplicate CRC-bound spool I/O. `SnapshotLayout.describe` re-implements `positions()`. `inspect_sources.py:220-240`'s manual `LinkSpanSummary` fold and the identical span-classification tails at `source_inventory.py:335-342` / `695-702` want a `merge()`. `check_lhalo_reachability`/`check_hdf5_reachability` share ≈70 % of their body. `_scatter_worker`'s variable-length tuple (`args[4] if len(args) > 4`) should always be a 5-tuple. The `ResidencyMeter = _Residency` public alias of a private class, and `_LedgeredSpills` reaching into `_Spills._sizes`, are seams held together by naming only.

**S9. Comments and docstrings.** `column_schema.py` carries ≈15 comments explaining *why a `TypeError` would otherwise escape*; one module-level note would do. `ctrees_hdf5.py`'s 180-line docstring is load-bearing (it is the only written C-parity contract) and should be kept, with its two mislabelled bullets fixed rather than the docstring shortened. `conversion_manifest.py:56` restates its import block. The slice-narrative docstrings (§3 P3) should become present-tense contract.

**Do not consolidate:** `run_generalisation_acceptance.ExternalSorter` intentionally re-implements `rank_sort.KeyedSorter` (C5 independence); the writer/validator duplicated structural checks (`_require_hard_links ↔ _link_kind_failures`, `_verify_v3_dataset_layout ↔ _v3_dataset_failures`) are deliberate two-implementation independence and should say so once. `LayoutEntry.selected` in the digest is redundant but frozen into every emitted `column_mapping_sha256`; removing it would change on-disk provenance. `_check_role_collisions`'s double-resolution exists only to order two error messages; leave it.

---

## 7. Runtime follow-on plan: architectural notes

`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md` is well-posed and consistent with VISION.md: it keeps the core physics-agnostic, treats `/schema` as producer metadata checked against package metadata (structural truth stays in `halo_properties.yaml`), gives retained state a single owner (the driver) and forbids repair. Its "Inspected state" table is accurate against the C source: `HORIZONTAL_HDF5_FORMAT_VERSION 2` (`read_horizontal_hdf5.c:67`), the `INT_MAX` refusal (`horizontal_driver.c:685`), `count_fof_subhalos` walking `mimic_tree_get_NextHaloInFOFgroup` (`halo_evolution.c:66`), `get_virial_mass` reading `FirstHaloInFOFgroup` (`virial.c:48`), plus the driver's own FoF uses at `horizontal_driver.c:417, 435, 476, 489, 924`. The PM's conclusion therefore holds: **R0-2 has one option, (a), executable as written**; (c) would strand at least five accessor call sites and (b) touches `binary.c`'s direct `fread` into `RawHalo`.

Four things a senior architect should push back on before recording Gate R0:

1. **R0-4(a)** widens *vertical-only* index code to int64 for uniformity, growing `struct HaloAuxData` on the hot vertical path with no functional need; the plan measures but does not bound the cost. Make (b) the recommendation or add an acceptance ceiling.
2. **R0-10**'s one-package-per-(simulation, source format) rule yields five near-mirror packages. The plan should say whether `converter_columns.yaml` (or the emitted `/schema`, which carries exactly this information) can generate the horizontal package's `halo_properties.yaml`; otherwise it is the metadata duplication VISION warns against.
3. **R0-7(a)** explicitly accepts reworking retention later; given the ≈4.9 TB largest-slab estimate, a short joint design note before its Slice 4 is cheaper than the rework.
4. **Slice 6's four-leg bitwise parity gate** relies on `sage16`, whose vertical baseline already carries ≈0.1 % chaotic threshold flips; "bitwise, no tolerance" on `sage16`/dynamic may be unachievable for reasons unrelated to gaps. Pre-declare how a non-gap divergence is adjudicated (the `mimic-scientific-method` skill's chaos-vs-bug discipline).

---

## 8. Process observations

- **The stash hazard is still live.** `git stash list` shows `stash@{0}: On feature/ctrees-snapshot-reader: slice4-revision4-work-preserved-for-revision5` plus twelve older WIP stashes on `main`. It bled into the working tree twice during the run. Since `feature/ctrees-snapshot-reader` is an ancestor of `main`, ask whether it can be dropped; if not, at least rename or move it out of position 0.
- **Surface control worked and has a known blind spot.** Every real defect the panel found was inside a slice's surface; both new documentation-of-record defects (F11, F12) were in files no later slice could touch. A post-run "files nobody owned" sweep should be a standard closing step.
- **The evidence is honest.** The acceptance document names real, sampled, fixture and unavailable populations separately, states measured versus extrapolated figures, and never equates conversion with runtime support. Its one weakness is the uncommitted supplementary scripts (§3 P3).
- **This document** is the record of the review and, through its status block, of what the follow-up implemented; the operational notes it cites under `.pm/runs/` and `archive/test-logs/` are gitignored working files, and every fact taken from them is restated here.

---

## Appendix: coverage

- **Scope:** every file in `git diff --stat f5cc6004..HEAD` (128 files) was assigned to and read by exactly one review group, plus one hop outward: `read_ctrees_hdf5.c`, `read_ctrees_ascii.c`, `binary.c`/`raw_halo_defs.h`, `read_horizontal_hdf5.c`, `horizontal_driver.c`, `halo_evolution.c`, `virial.c`, `generate_properties.py`, `crosscheck.py`, `subset.py`, the seven `halo_properties.yaml`/`simulation_info.yaml` pairs.
- **Requirements checked against:** plan contracts C1–C5, every slice's acceptance criteria, VISION.md, STYLE-GUIDE.md, the PM notes of both runs and the twelve per-slice assessments.
- **Dimensions:** correctness, boundary and invalid input, state/lifetime/crash recovery, interfaces and data contracts, numerical and scientific validity (cast order, bit-exactness, int64 bounds, signed zero, non-finite handling), performance at documented scales, robustness (symlinks, path handling), tests and oracle independence, observability, portability (macOS/Linux, strict C warnings), maintainability, documentation.
- **Not done:** real-data acceptance routes were not re-executed (Slice 11's evidence stands on its record); the differential lint skill was not re-run; full-Uchuu production data remains unavailable to anyone.
