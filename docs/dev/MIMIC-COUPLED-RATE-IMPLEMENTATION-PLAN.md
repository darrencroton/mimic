# Mimic Coupled Rate Implementation Plan (Step 7, Stage 1)

**Purpose:** Build the rate mode of the coupled rate formulation in Mimic's core: a fourth FoF dispatch mode whose modules declare conservative transfers that the core integrates as one system with error control, a conservation ledger, provenance, a model-neutral fixture and its batteries, proven on that fixture and changing no existing output.

**Status:** Planning contract, revision 2 (2026-11-12: revision 1 reviewed by an independent two-reviewer panel, Codex and Opus; revision 2 applies their findings). Thirteen Mode B slices on one feature branch; none has started.

**Planning baseline:** `39a8d28e` on `main` (v1.6 plus the bench and the `docs/dev` revisions), clean tree. Every `path:line` anchor below was read at that commit. PM binds a run to the SHA-256 of this file's bytes; recheck drift against the anchors before executing a slice, and never silently rebase a contract onto a changed interface.

**Owner:** [Development pathway, step 7](MIMIC-DEVELOPMENT-PATHWAY.md#the-ordered-road). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Requirements: [`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md) (the brief), which this plan corrects where its design section says so. Order of work: [`AGENTIC-SCIENCE-ARCHITECTURE.md`, stage 1](AGENTIC-SCIENCE-ARCHITECTURE.md#the-road). Evidence of record, written by Slice 12: `docs/dev/MIMIC-COUPLED-RATE-ACCEPTANCE.md`.

---

## Decisions for the owner

Rows 1 to 4 were taken by the owner on 2026-11-12. Rows 5 to 19 are taken by this plan and are frozen unless the owner changes them before the run starts; an alternative chosen here changes the slices named in [Design](#design).

| # | Decision | Taken by | Choice | Alternative and its cost |
|---|---|---|---|---|
| 1 | **Implicit scheme in stage 1** | Owner | **It ships.** Explicit is the default; the linearly implicit scheme is its own slice (Slice 7) behind the same interface | Explicit only, no scheme key: campaign 4b reopens the core if its fork proves stiff |
| 2 | **Names** | Owner | Mode string `process_rates`; enumerator `PROCESSING_MODE_RATES`; family `MODULE_CALLBACK_FAMILY_RATE`; callback `<module>_rates` bound to the `struct Module` member `rates`; context `struct RateContext`; property key `reservoir:`; `module_info.yaml` key `rates:`; run-file section `modules.rate_integration` with keys `scheme` (`explicit` or `implicit`), `relative_tolerance`, `absolute_tolerance`, `max_steps_per_segment`, `max_rejections_per_step` | Other names: each is user-facing and recorded in output, so a later rename is a migration |
| 3 | **Rows with different elapsed times** | Owner | **Own clock, the donor sets the amount**, in normalised segment time ([3.4](#34-time-segments-and-mixed-elapsed-times)) | A common span for every row: a row whose `dT` differs from the central's evolves for the wrong time (0.13 departure from the published semantics in the reference test) |
| 4 | **The generator is edited; the gates treat its hash as provenance** | Owner | `scripts/generate_properties.py` emits the reservoir table, and the identity gates' shared comparison treats `source_md5` as build provenance (Slice 1): a standing rule, not an allowance for this plan. Chosen as the robust long-term fix: the gate's sensitivity to the generator's bytes is latent today | A second, unhashed generator for the table: a second freshness mechanism, and the latent sensitivity stays |
| 5 | **Explicit scheme** | Plan | **SSPRK(4,3)**, four stages, SSP coefficient 2, embedded weights (1/4, 1/4, 1/4, 1/4), in place of the brief's three-stage candidate | SSPRK(3,3) with embedded weights (1/3, 1/3, 1/3): a third more law evaluations where positivity limits the step, about 6% fewer where accuracy does |
| 6 | **External ends are transfer ends, not properties** | Plan | Property kinds are `reservoir`, `tracer` and `observer`; a source is a transfer `from: external`, a sink a transfer `to: external`; per-total source and sink accumulators are solver state per segment. A model that wants a cumulative inflow in output declares an observer | An `external` property kind with accumulator properties: schema fields in every package for bookkeeping the solver already holds |
| 7 | **Slaved sources one level deep** | Plan | A slaved source is an external source into a different conserved total, its amount a dimensionless coefficient times a named master transfer's flux. Slaved sources are one level deep at the flux level: the master is a transfer out of a reservoir, never a source or another slaved source. A slaved source makes its recipient's total depend on its master donor's total; the totals are ordered so that every such dependency points forward, and a dependency cycle among totals is refused at generation and at startup ([3.2](#32-module-metadata-rates)) | Chains of slaved sources with a topological sort over fluxes: an ordering pass for a need no classified prescription shows |
| 8 | **Fixture state reaches output** | Plan | The seven test-only fixture properties are `output: true`; every test build of every package gains seven zero-valued `double` columns; the two SAGE physics-baseline tests expect the baseline's names followed by the fixture output fields. No baseline is regenerated and no production output changes | Log markers with hex floats and a bespoke comparator: no schema change, but the standard comparators and range checks never see the fixture |
| 9 | **Provenance is conditional** | Plan | Every new HDF5 attribute, dataset and row is written only when a rate phase is configured; `hdf5_format_version` is not bumped; `MinDynamicSubsteps` stays in the backlog | Unconditional records: a second gate allowance for candidate-only names, a version bump and churn in every older reference |
| 10 | **Canonical order** | Plan | State entries by (workspace row index among active rows at segment start, reservoir table index); fluxes by (donor row, global transfer index), the global index sorting every compiled rate module's transfers by (module name, declaration order) at generation time; every order-sensitive sum runs in that order. Nothing uses run-file order, registration order, `HaloNr`, `UniqueGalaxyID`, pointer values or `ws->identity` | A persistent birth key per record: a new output field in every package |
| 11 | **Where documentation lands** | Plan | The slice that adds a key documents it in the reference table that lists such keys and in the owning skill; the processing-model narrative, `docs/VISION.md`, `AGENTS.md`, `CHANGELOG.md`, the pathway and the sweep for false statements land in Slice 13. Between Slices 3 and 13 some documents of record lag the branch; nothing merges before Slice 13 | Each slice rewrites every document it makes stale: VISION edits ahead of the behaviour, contrary to VISION's own rule |
| 12 | **No per-file compiler flag** | Plan | The integrator is not built with `-ffp-contract=off`; each transfer's amount is computed once, stored and applied to both ends; conservation is asserted to a rounding tolerance; bit-identity is claimed per binary and across builds of the same source by the same compiler, as the existing gates assume | A per-object flag in the `Makefile` and `tests/unit/run_tests.sh`: two hand-kept rules, and still no cross-compiler claim |
| 13 | **The fuzzer is untouched** | Plan | `scripts/fuzz_pipeline.py` builds pipelines from `models/<model>/modules` only, and no package gains a rate module here | Teach the fuzzer rate phases: work with nothing to fuzz |
| 14 | **Where the identity legs run** | Plan | Cross-driver identity on the real mini-Millennium data in both layouts (what `make tests-reference-identity` already needs); task-count and chunk-count identity on the committed `forest_blocks` fixture; record creation under one driver only, because a created record's `UniqueGalaxyID` differs between drivers | Committed fixtures throughout: their groups are a few rows, too small to exercise coupled components |
| 15 | **Failure output** | Plan | A failed segment ends the run through `FATAL_ERROR` with a one-screen message naming the cause, and writes the component's segment-start state to `<output_directory>/rate_failure_task<N>.txt`, N the MPI task number (0 in a serial run); no replay tool | A replay tool: a new tool and tests for what the dump and a unit test reproduce |
| 16 | **Step statistics** | Plan | Integer histograms and counters, merged across MPI tasks by integer sum and maximum, written to the master file's `RunProperties` at run end and summarised in one `INFO_LOG` line; binary-output runs get the log line only | Floating-point summaries: values that depend on reduction order and task count |
| 17 | **The byte gate's reference** | Plan | `REFERENCE_COMMIT=39a8d28e`, the commit before this plan, for every slice | Re-anchoring per slice: a moving reference hides cumulative drift |
| 18 | **The purity probe is a run-file switch** | Plan | `modules.rate_integration` takes a sixth key, `check_purity` (`true` or `false`, default `false`), recorded in output with the others. The brief asked for a probe in test builds; a test executable carries no compile-time test marker today (only the unit runner defines `MIMIC_TEST_BUILD`), and a switch lets a campaign check the laws of a real model in any build | A compile-time probe: a new definition in the `Makefile`'s test-build flags, which would also switch on unrelated `#ifdef MIMIC_TEST_BUILD` hooks in the executable |
| 19 | **Central reads are declared** | Plan | `rates.reads_central` (default `false`); the central view is NULL unless declared, and a declaring module makes the group one component ([3.3](#33-the-rate-callback), [3.9](#39-dispatch-domain-purity)) | Always expose the central view at its segment-start state: a hidden lag that depends on nothing the metadata shows |

---

## Outcome and Limits

**Delivered when the plan closes:** a fourth FoF dispatch mode, `process_rates`; reservoir, tracer and observer declarations in property metadata and a generated reservoir table; transfer declarations in `module_info.yaml` and generated transfer tables; a core integrator with two schemes behind one interface; a conservation ledger over rate segments and discrete phases; step statistics and provenance in output; a model-neutral rate fixture; `make tests-rate`; the documents of record; and the acceptance record.

**Limits, stated so they are not overclaimed:** no model package gains a rate module and no production output changes by a byte; no located events (jumps fire at segment boundaries); no auxiliary state in rate phases; one rate phase per run file; a transfer ends on its own record or on the FoF central only; no production cost figure; bit-identity per binary; cross-driver identity proven for tree rows only; conservation to a stated rounding tolerance; discrete changes to a conserved total are attributed per dispatch unit, not per by-galaxy module; the two schemes' tolerances are not comparable, because the implicit scheme has no local extrapolation.

**Where the brief's gate and its open decisions are settled.**

| Brief item | Settled by | Design |
|---|---|---|
| Gate 1: every fixture problem meets its criterion | Slices 5, 6 and 7 (hand-built), 8, 9 and 10 (real dispatch) | [3.12](#312-the-proving-problems) |
| Gate 2: permutation, driver, task-count and chunk-count identity | Slice 11 | [3.9](#39-dispatch-domain-purity), decisions 10 and 14 |
| Gate 3: the ledger closes and a planted imbalance is detected | Slices 5, 6, 7 and 10 | [3.7](#37-the-ledger) |
| Gate 4: failures exit non-zero, name the cause, commit nothing | Slices 5 and 7 (unit), 8 and 9 (run) | [3.5](#35-the-explicit-scheme), decision 15 |
| Gate 5: every existing package and gate green with the mode unused | Every slice's byte gate; Slice 12 in full | Decisions 4 and 9 |
| Gate 6: the cost of the mode measured on the fixture | Slice 12 | none |
| Gate 7: the documents of record | Slice 13, with keys documented as they land | Decision 11 |
| Plan decision 1: reservoir keys and the generated table | Slice 2 | [3.1](#31-property-metadata-reservoir) |
| Plan decision 2: transfer, source, observer and slaved-source declarations and their validation | Slice 4 (and Slice 10 for `changes_totals`) | [3.2](#32-module-metadata-rates), decisions 6, 7 and 19 |
| Plan decision 3: the callback's signature and context | Slice 3 (declared), Slice 8 (wired) | [3.3](#33-the-rate-callback) |
| Plan decision 4: tolerance formula, defaults, budgets, failure diagnostic | Slice 5 (numerics), Slice 8 (keys and fatal message), Slice 9 (failure file) | [3.5](#35-the-explicit-scheme), decision 15 |
| Plan decision 5: the canonical ordering key, proved driver-independent | Slice 8 (assembly), Slice 11 (row-order and cross-driver legs) | Decision 10 |
| Plan decision 6: rows with different elapsed times | Slice 5 (unit), Slice 8 (dispatch) | [3.4](#34-time-segments-and-mixed-elapsed-times), decision 3 |
| Plan decision 7: the solver workspace's memory category, owner and free point | Slice 8 | [3.9](#39-dispatch-domain-purity) |
| Plan decision 8: the implicit linear solve | Slice 7 | [3.8](#38-the-implicit-scheme) |
| Plan decision 9: new output records and the byte gate | Slices 1 and 10 | [3.10](#310-provenance-and-step-statistics), decisions 4 and 9 |

---

## Frozen Facts and Evidence

The facts this plan designs against, each read at the baseline. A slice that finds one wrong stops rather than silently rebasing. Nothing in this plan was tested by building or running Mimic.

**Mode machinery.**

- The modes live in one C table with a count assertion (`src/core/processing_modes.c:37-49`) over `enum ProcessingMode` (`src/core/module_interface.h:99-105`); there are two callback families (`:115-118`). `check_module_callback_families()` switches on the family with a fatal `default:` (`src/core/module_registry.c:258-290`), so a family without a case fails at registration.
- `validate_phase_processing_modes()` refuses any non-FoF family in a FoF phase, naming the phase and module (`src/core/module_registry.c:605-615`).
- `struct PhasePlan` holds three lists (`src/core/module_registry.c:785-795`) and `build_phase_plan_visitor()` fills them from three `collect_phase_mode()` calls without checking that they cover every entry (`:863-881`), so an unplanned mode would be dropped silently.
- `execute_phase()` makes two passes, full-halo then by-galaxy (skipping rows with `galaxy == NULL` or `Type == 3`), inside `begin_phase_event_dispatch()` and `end_phase_event_dispatch()`, with per-event dispatch nested in emission (`src/core/module_registry.c:1678-1762`, window at `:1696` and `:1761`, skip at `:1740`).
- `module_emit_event()` returns 0 and drops the event when no phase dispatch is active, and never consults the running-callback kind (`src/core/module_registry.c:1197-1201`); `module_create_record()` refuses any kind but full-halo (`:1396-1403`). Two `switch` statements over `enum RunningCallbackKind` (`src/core/module_registry.h:286-294`) have no `default:`: `running_callback_description()` (`src/core/module_registry.c:1373-1390`) and `callback_kind_phrase()` (`src/core/snapshot_collectives.c:49-67`).
- `scripts/module_modes.py` mirrors the table (`CALLBACK_FAMILIES` `:67-80`, `PROCESSING_MODES` `:103-108`, `STANDALONE_FALLBACK_MODES` `:115-119`); `tests/integration/test_snapshot_module_schema.py:152-174` enforces the mirror, requires Python order to equal enum order, derives the C family suffix from the Python key and pins the field list to `["process", "process_snapshot"]`; the module generation hash covers `module_modes.py` (`scripts/generate_module_registry.py:665-668`); callbacks bind by name from `CALLBACK_FAMILIES` (`:786-790`).

**Time.**

- Each row carries its own `dT` (`src/core/inheritance.c:103` for new halos, `:168` for inherited rows); the substep count is group-wide (`src/core/halo_evolution.c:175-184`); the context interval and substep length are the central's (`:168-173`, `:189`), and the context's midpoint time is set per substep (`src/core/module_registry.c:1814-1819`). The published models' helper divides the row's own `dT` by the substep count, skips the first-snapshot sentinel (`SnapNum < 0` and `dT <= 0`) and treats any other non-positive `dT` as invalid (`models/sage16/shared/time_parity.h:62-71`), with the row's midpoint time at `:105`. The mixed-`dT` tests live in `models/sage16/modules/_tests/test_unit_mixed_dt_parity.c` and `models/sage26/modules/_tests/test_unit_mixed_dt_parity.c`, not in `tests/unit/` (the brief's path is wrong).

**Row order and identity.**

- Both drivers walk the FoF chain head first and call the one `process_halo_evolution()` (`src/core/build_model.c:149-162`, `src/core/horizontal_driver.c:1180-1193`), building each slice through the one `inherit_descendant_halos()` (`src/core/inheritance.c:148`).
- The FoF central is the first `Type == 0` row found by scan, not necessarily row 0 (`src/core/halo_evolution.c:220-236`); created rows are appended at the workspace tail (`src/core/module_registry.c:1611-1619`). Cross-driver comparison covers tree rows only (`scripts/compare_cross_format_identity.py:33-42`); no test compares in-group row order across drivers; a created record's `UniqueGalaxyID` differs between drivers (`src/core/module_interface.h:424-436`), so no row carries a driver-independent identity covering created records.

**Properties and generation.**

- A property needs `name`, `type`, `units`, `description` and `output` (`scripts/generate_properties.py:215`); `validate_property()` checks named keys only, so an unknown key is ignored (`:211-303`); `init_repeat` rules are at `:286-303`. No runtime table of `struct GalaxyData` offsets exists; the only generated offset tables index `struct HaloOutput`.
- `compute_yaml_hash()` hashes the generator's own bytes and the property YAMLs (`scripts/generate_properties.py:664-683`), and the digest is written into `metadata/output_schema.json` as `source_md5` (`:1676`). The cross-format identity test already treats that key as provenance: `SCHEMA_PROVENANCE_KEYS = {".source_md5"}` (`simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py:164`), excluded before its schema comparison (`:466`). The generator was last edited on 2026-10-05 (`6bcc7811`), before the byte gate existed (`2c0eea4b`, 2026-10-21).
- Generated code is gitignored (`.gitignore:37-39`). Test-fixture properties are appended to the model's only in test builds (`scripts/generate_properties.py:1948`, `scripts/discovery.py:225-234`), after every model galaxy property, and `struct HaloOutput` lists halo then galaxy outputs in YAML order (`scripts/generate_properties.py:841-857`). `scripts/check_generated.py` omits test properties (`:43`), so it reports false drift under a test build; its file list is `:46-63`. Inheritance resets `init_repeat` fields once per snapshot (`src/core/inheritance.c:23-30`).

**Modules and fixtures.**

- Neither `scripts/generate_module_registry.py` nor `scripts/validate_modules.py` rejects an unknown `module_info.yaml` key (no allow-list in either).
- The generic source search excludes `test_*.c` (`Makefile:112`), so each fixture needs a `SOURCES +=` line in the test-build block (`:122-130`); `make TEST_BUILD=yes` exports `MIMIC_TEST_BUILD` to the generators (`:103-106`) but adds no `-DMIMIC_TEST_BUILD` to `CFLAGS` (`:154-159`), which only `tests/unit/run_tests.sh:118` defines.
- `validate_modules.py` sees fixtures only under `MIMIC_TEST_BUILD` (`scripts/validate_modules.py:13-16`); `make validate-build` is `lint-parameters` (`Makefile:331-334`), whose exit 2 for a declared but unused parameter fails the build (`scripts/lint_parameter_usage.py:247`, `Makefile:616-619`). The fuzzer reads `models/<model>/modules` only (`scripts/fuzz_pipeline.py:158-166`).

**Gates and suites.**

- The byte gate compares `metadata/` files byte for byte apart from `version_info.json` and path prefixes (`tests/framework/parity_gate.py:1595-1628`), offers a `metadata_delta` hook (`:1022-1023`) that `compare_metadata_file()` calls after its `version_info.json` rule and ahead of its identical-bytes and path-prefix rules (`:1609-1617`) and that `tests/manual/test_reference_identity.py:497-499` does not pass, carries each side's build digest as `OutputRun.source_md5` (`tests/framework/parity_gate.py:1302-1310`), requires identical HDF5 object sets (`:1505-1511`) and attribute-name sets (`:1440-1446`), and refuses uncommitted edits under `src`, `scripts`, `models`, `simulations` and the `Makefile` (`:997-998`). It runs `{halos-only, sage16} × {mini-millennium, mini-millennium-horizontal} × {fixed, dynamic}` (`tests/manual/test_reference_identity.py:109-114`, `:166-171`) from the committed run files, which must be byte-identical at both commits (`:201-206`).
- `make tests-distributed` builds production binaries of four models (`tests/manual/test_distributed_identity.py:42`, `:108`); `make tests-reference-identity` builds production binaries too, so neither can run a test-build fixture.
- `tests/unit/run_tests.sh:133-134` and `tests/unit/tools/build_topology_dump.sh:86-99` link hand-kept source lists; the second is what `make tests-converter` exercises.
- The two SAGE physics-baseline tests pin output field names (`models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py:189-193`, `models/sage26/modules/_tests/test_scientific_sage26_physics_baseline.py:258-264`); the zero-value warning skips a field whose `sentinels` contain 0 (`tests/scientific/test_scientific.py:228-234`). `make tests` leaves a test-build `./mimic` (`Makefile:101-102`); the batteries restore generated code for the caller's pair and leave the executable for `make` to rebuild (`tests/manual/run_snapshot_global_battery.py:206-218`).
- No all-packages loop exists; `models/*/input/` holds 51 run files declaring 50 distinct `(model.name, simulation.name)` pairs (`sham_micro-uchuu-ascii-horizontal-realdata.yaml` repeats a pair).

**Run file.**

- `modules:` accepts five keys through an inline chain and rejects others (`src/core/read_parameter_file.c:1414-1420`); phase names that equal a reserved key are refused (`:1318-1327`); the top level reads named keys and has no whitelist (`:218-264`). `tests/framework/harness.py:712-726` rebuilds `modules` from scratch, so any other `modules` key in a reference run file is dropped.

**Provenance.**

- Two HDF5 writers: `store_run_properties()` for the master file (`src/io/output/metadata_hdf5.c:523`, called at `src/io/output/master_hdf5.c:263`) and `write_perfile_metadata()` per partition (`metadata_hdf5.c:480`, called at `src/io/output/hdf5.c:152`); the master is written on rank 0 after a barrier and before `module_system_cleanup()` (`src/core/main.c:438-450`, `:477`).
- `EventContracts` is written only when contracts exist (`metadata_hdf5.c:406-418`) and `post_snapshot` rows only when that phase is configured (`:280-286`); `hdf5_format_version` is `"1.2"` (`:127-130`). The bench requires `RunProperties/Parameters` names to equal its template's (`bench/mimic-bench/mimic_bench/capture.py:606-613`).

**Memory.** Categories are fixed (`src/util/memory.h:7-15`); the phase event buffer grows to its high-water mark (`src/core/module_registry.c:1023`) and is released in `module_system_cleanup()` (`:1930`), which runs before the leak check (`src/core/main.c:477`, `:505`). `src/util/integration.c` is cosmology quadrature (`:1-8`), so the integrator does not belong there.

**Measured numerics.** The figures quoted in [3.5](#35-the-explicit-scheme) to [3.8](#38-the-implicit-scheme) and in [3.12](#312-the-proving-problems) were verified in a scalar Python reference implementation outside the repository, in IEEE binary64 without fused multiply-add. They were not verified in C.

| Claim | Reference result |
|---|---|
| SSPRK(4,3) order and SSP coefficient | Order exactly 3 (embedded exactly 2); C = 2 for both weight sets |
| Cost against SSPRK(3,3) | 225 against 337 evaluations at K·Δ = 100; 6,677 against 10,014 on the stiff pair; 5 to 6% more at rtol 1e-3 to 1e-6 when accuracy limits |
| A start-of-step cap alone is unsafe | Sink at 1 + 50·B fed by a source of 10 into B commits A = −333.3 |
| Rounding at the cap | 32 of 11,075 steps with a negative stage at dt·K = C; 0 of 12,000 at σ = 0.9 |
| Ledger bound | Drift at most 1.3e-3 of tol_F over 1e3 to 1e6 steps; a planted 1e-9 relative imbalance detected, 1e-12 not |
| Mixed Δ | The published per-row `dT/N` form converges to the normalised-time solution at first order (2.8e-2 to 2.7e-5 for N = 10 to 1e4); a common span departs by 0.13 |
| Implicit solve | GTH block solve non-negative and about 5e-16 componentwise against an exact rational solve at dt·k up to 1e20, where dense LU goes singular; flat cost per galaxy for N = 10 to 4,000 |
| Implicit stiff pair | 38 steps at every exchange rate from 1e3 to 1e12 on the slow manifold, relative error about 3e-2 at rtol 1e-3 |

---

## Design

The contracts the slices implement. Subsections keep the numbers the decisions and slices cite. Terms used throughout: u = 2⁻⁵³; Δ_r is row r's segment length; an entry is one reservoir or tracer of one active row; K_i is entry i's total specific outflow in τ units (Δ of its row times the sum of the coefficients out of it); K_max is defined in [3.5](#35-the-explicit-scheme); L is the largest number of inflow plus outflow terms on any one entry; N_acc is the accepted steps of a segment; n_F is the number of entries in conserved total F; the error of a test problem is the largest absolute difference between a computed entry and its closed form at τ = 1.

### 3.1 Property metadata: `reservoir:`

A galaxy property may carry a mapping `reservoir:` with keys `kind` (`reservoir`, `tracer` or `observer`), `total` (the conserved total it belongs to; required for `reservoir` and `tracer`, refused for `observer`) and `parent` (required for `tracer`, refused otherwise; names a property of kind `reservoir`).

The generator refuses each of these with a message naming the property: an unknown key in the mapping; the mapping on a halo or core property; a type other than scalar `double`; `init_repeat` on a `reservoir` or `tracer`; a `tracer` whose parent is missing, is not a `reservoir`, or shares its total; two tracers of one parent in the same total; a total that holds both reservoirs and tracers; two members of one total whose `units` strings differ. A total is a name, and it exists when at least one `reservoir` or `tracer` names it. A tracer's total is distinct from its parent's: metal mass is part of gas mass, so it does not add to the baryon total.

Generated (a new file under `src/include/generated/`, listed in `scripts/check_generated.py`): an index enumeration with one symbol per reservoir entry; a table of `{name, offsetof(struct GalaxyData, field), kind, parent index or -1, total index or -1}` in property YAML order; the total names; and the counts. A build with no reservoir, which is every production build today, generates a form that compiles clean under the project warnings.

### 3.2 Module metadata: `rates:`

```yaml
rates:
  reads_central: false          # true: the law reads the FoF central's trial state (default false)
  transfers:
    - name: cooling             # C identifier, unique within the module
      from: RateFixtureHot      # a reservoir, or the word external
      to: RateFixtureCold       # a reservoir, or the word external
      to_record: self           # self (default) | central
    - name: yield
      from: external
      to: RateFixtureColdTracer
      slaved_to: test_rate_formation.star_formation
  observers:
    - property: RateFixtureFormed
      transfer: star_formation
```

A law returns one coefficient per declared transfer, always finite and non-negative. Its meaning depends on the declaration:

| Declaration | Kind | Coefficient | Flux in the row's own time |
|---|---|---|---|
| `from` and `to` both reservoirs | Internal transfer | Specific rate, inverse time | Coefficient times the donor's content |
| `to: external` | Sink | Specific rate, inverse time | Coefficient times the donor's content |
| `from: external`, no `slaved_to` | Source | Absolute rate, content per time | The coefficient |
| `from: external` with `slaved_to` | Slaved source | Dimensionless | Coefficient times the master transfer's flux |

Each of these is refused at generation, validation or startup: `from` and `to` both `external`; an unknown key; `reads_central` that is not a boolean; an end that is not a declared property of kind `reservoir` (a `to` may be a `tracer` only when `from` is `external`; a `from` is never a tracer or an observer); an internal transfer (both ends reservoirs) whose two reservoirs belong to different conserved totals; `to_record: central` with `to: external`; a duplicate transfer name in a module; `slaved_to` naming anything other than a transfer with a reservoir donor, or one whose donor's total equals the recipient's total; a dependency cycle among totals (below); an observer naming a property not of kind `observer` or a transfer the module does not declare; a module that declares `rates:` without advertising `process_rates`, or the reverse; a standalone `.c` module advertising `process_rates`. Movement between totals is a sink in one and a slaved source in the other, never an internal transfer.

**Totals are ordered.** A slaved source makes its recipient's total depend on its master donor's total, and a tracer total depends on each of its tracers' parents' totals. A total holds reservoirs or tracers, never both, so the second kind of dependency cannot close a cycle. The conserved totals are ordered so that every dependency points forward; a cycle among totals is refused at generation (over all compiled rate modules) and at startup (over the configured ones), with a message naming the totals. The explicit scheme evaluates slaved sources after the transfers and needs no order; the implicit scheme solves the totals in that order.

**Central reads are declared** (decision 19). `reads_central: true` declares that the module's law reads the FoF central through `ctx->central` ([3.3](#33-the-rate-callback)); the default is `false`.

`to_record: central` puts the recipient on the FoF central (`ctx->central_index`), the only cross-record end. On the central itself such a transfer acts on its own record, and when its two ends are then the same reservoir it contributes nothing.

**Tracers move without declaration.** For every transfer out of a reservoir, each tracer of that reservoir moves at the same coefficient into the recipient's tracer of the same total. Where the recipient is external or has no tracer of that total, the tracer amount leaves its total and the ledger records it as a sink.

**Discrete modules declare total changes.** A module of any existing mode that changes a conserved total declares it with a top-level key `changes_totals: [<total>, ...]` (Slice 10).

**Generated:** per module, a table of its transfers and observers with reservoir indices, its `reads_central` flag, a coefficient index constant per transfer (the slot the law writes), and the `rates` callback binding. Because the tables name reservoirs by the generated index symbols, a removed reservoir is a compile error, and the core re-checks every configured transfer's end kinds against the reservoir table at startup, so stale generated metadata cannot run.

### 3.3 The rate callback

```c
int <module>_rates(const struct RateContext *ctx, const struct Halo *halo, double *coefficients);
```

The callback is called once per active row per law evaluation; rows with `galaxy == NULL` or `Type == 3` are skipped, as the by-galaxy pass skips them. `halo` is a core-owned scratch copy of the row whose `galaxy` pointer addresses a scratch `struct GalaxyData` holding the trial state: the row's galaxy copied at segment start, with reservoir and tracer fields overwritten at each evaluation. The law reads properties as typed fields, as every module does today, and never sees the committed workspace. `coefficients` has one slot per transfer the module declares, zeroed before the call.

`struct RateContext` carries the snapshot number and redshift, the row's own time at the evaluation point, `ctx->central`, and the read-only configuration. `ctx->central` is a `const` pointer to the scratch copy of the FoF central at the trial state, non-NULL only for a module that declares `reads_central: true` ([3.2](#32-module-metadata-rates)); for every other module it is NULL. A declaring module makes the group one component ([3.9](#39-dispatch-domain-purity)), so a law in the per-galaxy domain cannot see another galaxy's state. The context carries no solver step, substep index or substep count. A non-zero return, or a coefficient that is negative or not finite, fails the segment naming the module and transfer. `const` is shallow in C, so the purity probe ([3.9](#39-dispatch-domain-purity)), not the signature, is what detects a law that writes.

### 3.4 Time, segments and mixed elapsed times

A substep is a segment. Segment time is normalised: τ runs from 0 to 1. Row r has its own segment length Δ_r, the row's `dT` divided by the group's substep count, and every flux is scaled by its donor row's Δ; a source is scaled by its recipient row's Δ, and a slaved source inherits its master's scaling. One amount per transfer is applied to both ends, so a cross-record transfer conserves whatever the two rows' Δ.

The law's time for row r at τ is the row's own: `ctx->time + dT_r` less `(substep + τ) × Δ_r`, in the lookback convention `struct ModuleContext` uses. A row with Δ = 0 (the first-snapshot sentinel, `SnapNum < 0` and `dT <= 0`) donates nothing and its own laws and sources move nothing, but it still receives what a donor with positive Δ sends it, because a transfer's amount is set on its donor's clock and applied to both ends. Any other non-positive `dT` is fatal, as it is in the published models' time helper. When every Δ is zero the segment takes no step and evaluates no law. This is the continuous limit of what `sage16` does today, each object over its own `dT / num_substeps`, verified numerically.

### 3.5 The explicit scheme

SSPRK(4,3) in Butcher (increment) form:

```text
c    = (0, 1/2, 1, 1/2)
A    = [ 0                ]
       [ 1/2  0           ]
       [ 1/2  1/2  0      ]
       [ 1/6  1/6  1/6  0 ]
b    = (1/6, 1/6, 1/6, 1/2)                      third order
bhat = (1/4, 1/4, 1/4, 1/4)                      second order, embedded
d    = b - bhat = (-1/12, -1/12, -1/12, 1/4)     error weights
```

For each flux the stage combination is formed once, and that one number is subtracted from the donor entry and added to the recipient entry, at every stage and in the final update: `(x − Σ outflow amounts) + Σ inflow amounts`, each sum in canonical order (decision 10).

**Positivity** (C = 2, margin σ = 0.9). K_max, here and everywhere it is used (the a-priori cap, the stage check, the retry rule, the statistics), is the maximum of K_i over entries whose content is non-zero at the state in question: an entry with zero content cannot go negative, so it does not limit the step. With no such entry there is no cap (the cap is +∞). Before the first stage the step is capped at σ·C / K_max at the step-start state. After the laws are evaluated at each stage state, the step is rejected if step × K_max at that stage exceeds C. After each stage state and the final state are formed, the step is rejected if any entry is strictly below zero. No law is ever evaluated at a state with a negative entry, and nothing is clamped. After a positivity rejection the step becomes the smaller of half the step and σ·C over the largest K_max seen in the attempt.

**Error control.** The error vector covers reservoir and tracer entries only; observers and ledger accumulators are excluded. The weight is w_i = `absolute_tolerance` × S + `relative_tolerance` × max(|x_i| at step start, |x_i| at the trial end), where S is the larger of the entry's own galaxy's total over the non-tracer entries of the relevant total at step start and at the trial end, floored at 1e-12 times the component's total; a tracer uses its parent's total. Both tolerances are dimensionless. err = max_i |e_i| / w_i. A ratio with a zero numerator is zero, so a zero error contributes zero; a zero weight under a non-zero error, or a NaN, contributes +∞; a step is accepted when err ≤ 1.

**Controller.** factor = 0.9 × err^(−1/3), clamped to [0.2, 5], with no growth on the step after a rejection. The first step uses the Hairer and Wanner starting-step rule adapted to this norm and to the positivity cap, at the cost of one extra law evaluation per segment. An entry whose weight w0_i is zero is left out of d0, d1 and d2. When every entry is left out (the component's totals are all zero at segment start), dt0 = min(1, cap) with no probe and no extra evaluation, and the error control sizes the steps after it from the trial end's scale. So an all-empty component with no source takes one step and stays zero, and an all-empty component with a source is integrated from the first step. The last step is stretched by up to 10% to end exactly at τ = 1, otherwise a remainder shorter than two steps is split into two equal steps; τ is assigned 1.0 at the end, never accumulated.

```text
First step, at the segment-start state x0 with net rate F0 (inflow minus outflow per entry, in tau
units, from the first stage's evaluation, which is reused). Kmax(x) is over entries with non-zero
content; sigma / Kmax and cap(x) = sigma * C / Kmax(x) are +inf when no entry has content.
  w0_i = absolute_tolerance * S + relative_tolerance * |x0_i|
  E = { i : w0_i > 0 }                          zero-weight entries are left out
  if E is empty:                                every total is zero at segment start
    dt0 = min(1, cap(x0))                       no probe, no extra evaluation
  else, every max over i in E:
    d0 = max_i |x0_i| / w0_i        d1 = max_i |F0_i| / w0_i
    h0 = (d0 < 1e-5 or d1 < 1e-5) ? 1 : 0.01 * d0 / d1
    h0 = min(h0, 1, sigma / Kmax(x0))           a forward Euler probe, non-negative by this cap
    x1 = x0 + h0 * F0   (increment form)        F1 = F(x1), the one extra evaluation
    d2 = max_i |F1_i - F0_i| / w0_i / h0
    h1 = (max(d1, d2) <= 1e-15) ? 1 : (0.01 / max(d1, d2))^(1/3)
    dt0 = min(100 * h0, h1, 1, cap(x0))

Landing, with r = 1 - tau and cap = cap(x_n):
  dt = min(dt, cap)
  if r <= 1.1 * dt and r <= cap:   dt = r, and this is the last step
  else if r < 2 * dt:              dt = r / 2
```

**Fixed steps.** The interface also takes a fixed number of equal steps with the controller off. The positivity and negativity checks stay on, and a fixed step that fails one fails the segment. The order tests use it.

**Defaults and constants.** `relative_tolerance` 1e-4; `absolute_tolerance` 1e-7; `max_steps_per_segment` 100000 (attempts, accepted and rejected); `max_rejections_per_step` 30 (consecutive). Compile-time constants: the controller constants, σ, the scale floor and a minimum step of 2⁻⁴⁶ of the segment. The step sequence is a pure function of the segment-start state, the forcing, the parameters, the Δ values and the tolerances: the integrator starts cold in every segment and holds nothing between segments or components.

**Failure.** Causes: `bad_coefficient`, `step_budget`, `rejection_budget`, `step_underflow`, `ledger`, `tracer_bound`. `rejection_budget` is `max_rejections_per_step` consecutive rejections of one step. The integrator works on a gathered copy and writes back only on success, so a failed segment leaves the workspace untouched. The failure record names the cause, the row, the entry, the module and transfer, τ and the step. In a run, the failure file is `<output_directory>/rate_failure_task<N>.txt`, N the MPI task number (0 in a serial run), holding the component's segment-start state as hexadecimal floats, each row's Δ and the coefficients at the failing evaluation (decision 15).

### 3.6 Tracers, slaved sources, observers

A tracer's flux for a transfer is the transfer's coefficient times the tracer's own content, so no division by the parent occurs and an empty donor is well defined. An observer accumulates the committed amount of the flux it names, the same number that moved. A slaved source's stage flux is its coefficient times the master's stage flux, evaluated after the transfers.

**Tracer bound.** At segment end, tracer minus parent may not exceed 8u·(L + 2)·N_acc·Θ_p, where Θ_p, the parent's gross throughput over the segment, is its content at segment start plus the sum of the inflow amounts into it. Both schemes preserve the bound when every inflow to a tracer is matched by a parent inflow at least as large; a source or slaved source into a tracer can break that premise in the exact solution, so the failure message says whether the tracer has sources of its own.

### 3.7 The ledger

Per component, per segment, per conserved total F:

```text
| SUM_F x(end) - SUM_F x(start) - (Src_F - Snk_F) | <= tol_F
tol_F = 8 u [ (L + 2) N_acc + n_F + 2 ] ( SUM_F |x(start)| + Src_F + Snk_F )
```

Src_F and Snk_F are the segment's external amounts into and out of F, integrated by the same increments as the state. Under the implicit scheme L is the larger of that count and the number of entries in one galaxy's block, which covers the block solve's rounding. Verified: observed drift at most 1.3e-3 of the bound over 1e3 to 1e6 steps, and a planted relative imbalance of 1e-9 detected. A violation fails the segment with cause `ledger`.

**Discrete phases** (Slice 10). Only when a rate phase is configured, the core sums each conserved total over the FoF workspace's active rows before and after each dispatch unit of a discrete phase. A dispatch unit is one full-halo module's call together with the per-event consumers it triggers, or the phase's by-galaxy pass as a whole (that pass is galaxy-major, so its modules cannot be separated without a sum per call). A change beyond 8u·(n_F + 2)·Σ_F|x| in a full-halo unit is fatal unless that full-halo module, or a per-event module configured in the phase that subscribes to an event that module emits, declares the total in `changes_totals`; a change in the by-galaxy pass is fatal unless a by-galaxy module configured in the phase declares it. A declared change is counted in the step statistics. The granularity is a limit: among the by-galaxy modules of one phase, and between a producer and the consumers subscribed to its events, a declaration by one covers the unit.

### 3.8 The implicit scheme

Patankar–Euler with coefficients frozen at the step start: (I − dt·A(x_n))·x_{n+1} = x_n + dt·s(x_n), with A built from the declared coefficients only, never a Jacobian. The matrix is a column-diagonally-dominant M-matrix, so the solution is non-negative at any step and each total changes by sources minus sinks to rounding. First order.

**Solve.** Block lower-arrow: one block per galaxy, satellites in canonical order, then the central, which gathers the satellites' cross-record terms. Each block is factorised without pivoting and without subtraction, storing off-diagonal magnitudes and each column's excess separately (the Grassmann–Taksar–Heyman form), so work is linear in the number of galaxies and no matrix larger than one galaxy's block is formed. Totals are solved in the order of [3.2](#32-module-metadata-rates), each after every total it depends on: a parent total before its tracer total, and a slaved source's recipient total after its master donor's total, with slaved amounts on the right-hand side.

```text
One galaxy's block, entries 0..R-1, in tau units (every rate already carries its row's Delta).
Stored: off[i][j] = dt * (rate from entry j to entry i) >= 0 for i != j, and the column excess
e[j] = 1 + dt * (rates of entry j that leave the block: sinks and cross-record outflows).
The diagonal is never stored: it is e[j] + SUM_{i != j} off[i][j].

for k = 0 .. R-1:
  piv[k] = e[k] + SUM_{i > k} off[i][k]
  for i > k:   l[i][k] = off[i][k] / piv[k]                      0 <= l <= 1
  for j > k with off[k][j] != 0:
    for i > k, i != j:   off[i][j] += l[i][k] * off[k][j]        magnitudes only grow
    e[j] += off[k][j] * (e[k] / piv[k])                          excess of the Schur complement
forward:    y[i] = b[i] + SUM_{j < i} l[i][j] * y[j]
backward:   x[i] = (y[i] + SUM_{j > i} off[i][j] * x[j]) / piv[i]

Component: solve each satellite's block; add each satellite's cross-record terms,
dt * rate * x_satellite, to the central's right-hand side in canonical order; solve the central.
```

**How the kinds enter.** The right-hand side is x_n plus dt times the absolute sources at x_n. After the solve, each transfer's amount is dt × its coefficient at x_n × its donor's solved content; those amounts feed the sink accumulators, the observers and the ledger. A tracer total is solved after its parent total with the same coefficients, on the entries that carry the tracer, a transfer into an entry without the tracer counting as leaving the block. A slaved source's amount is its coefficient at x_n times its master transfer's amount, added to its recipient's right-hand side and to that total's source accumulator.

**Error estimate.** e = M⁻¹·(dt/2)·(F(x_{n+1}) − F(x_n)), which reuses the factors and the next step's law evaluation; the same norm; controller exponent 1/2; no positivity cap on the step. The implicit first step is dt0 = h0, where h0 = 1 if d0 < 1e-5 or d1 < 1e-5 and otherwise min(1, 0.01·d0/d1), with the same exclusion of zero-weight entries and dt0 = 1 when every entry is excluded; no probe state is formed and no extra law is evaluated, so no law ever sees a state the solve did not produce. The scheme integrates an ill-posed law without complaint, so it is never the default.

### 3.9 Dispatch, domain, purity

A phase under `modules.phases` whose entries are all `process_rates` is the rate phase. Refused at startup, before any `init()`: a phase mixing `process_rates` with another mode; `process_rates` in `pre_timestep`, `post_timestep` or `post_snapshot`; a second rate phase; a slaved source whose master's module is not configured in the rate phase; a dependency cycle among the totals of the configured rate modules ([3.2](#32-module-metadata-rates)); a `modules.rate_integration` section with no rate phase, or with an unknown key or an out-of-range value; a configured transfer whose ends the reservoir table does not hold as the kinds [3.2](#32-module-metadata-rates) allows. With a rate phase and no section, the defaults apply. The integration domain is fixed at startup from declared metadata only: the whole FoF workspace is one component when any configured transfer has `to_record: central` or any configured rate module declares `reads_central: true`; otherwise each galaxy is its own component. So a law in the per-galaxy domain cannot see another galaxy's state.

The rate pass runs inside `execute_phase()`'s existing event-dispatch window, under a new running-callback kind. `module_emit_event()` gains an explicit refusal for that kind ahead of its inactive-dispatch shortcut; `module_create_record()` already refuses any kind but full-halo. State is gathered at the start of each segment and written back at its end, so a discrete phase may change the number of rows between segments. The solver's workspace is one module-system-owned allocation in `MEM_UTILITY`, grown to its high-water mark and released in `module_system_cleanup()`, following the phase event buffer.

**Purity probe** (decision 18), run when `check_purity` is `true`. At the first law evaluation of each segment the laws are evaluated a second time in reversed canonical order; every coefficient must match bit for bit (compared as 64-bit patterns) and the scratch state must be byte-identical afterwards. A mismatch is fatal and names the module and transfer.

### 3.10 Provenance and step statistics

Written only when a rate phase is configured. Master file and per-partition files: a dataset `RunProperties/RateTransfers` with one row per configured transfer (module, transfer, from, to, to_record, kind, master), in global transfer index order (decision 10), built like `EventContracts`. Master file only: a group `RunProperties/RateIntegration` with attributes for the scheme, the four numeric keys, `check_purity` and one string naming the method and controller constants, and datasets of step statistics: a histogram of the per-segment maximum of K_max ([3.5](#35-the-explicit-scheme)) in whole-octave bins, taken from the binary exponent, with dedicated bins for zero, underflow and overflow; a histogram of accepted steps per segment by bit length; uint64 totals of segments, accepted steps, rejected steps and law evaluations; the exact maxima; and per-total counts of declared discrete changes.

Counts merge across MPI tasks by integer sum and maxima by maximum, so the statistics are identical for any task count, chunk count or driver. No floating-point sum is stored.

### 3.11 The rate fixture

Test-build properties in `src/module_system/test_fixture/test_properties.yaml`, all scalar `double`, `output: true`, initial value 0, `sentinels: [0.0]`, a declared range that includes 0, mass units: `RateFixtureHot`, `RateFixtureCold` and `RateFixtureStars` (kind `reservoir`, total `rate_fixture_mass`); `RateFixtureHotTracer`, `RateFixtureColdTracer` and `RateFixtureStarsTracer` (kind `tracer`, parents respectively, total `rate_fixture_tracer`); `RateFixtureFormed` (kind `observer`).

Fixture modules under `src/module_system/`, each a directory module with a README, compiled in test builds only:

| Module | Mode | Declares |
|---|---|---|
| `test_rate_supply` | `process_rates` | Source `infall` into `RateFixtureHot` on Type 0 rows, proportional to the halo's virial mass; source `infall_tracer` into `RateFixtureHotTracer` at a fixed fraction of it below one; `cooling` from Hot to Cold |
| `test_rate_formation` | `process_rates` | `star_formation` from Cold to Stars with a threshold law (zero below a critical content, a continuous rate above it); observer `RateFixtureFormed` |
| `test_rate_feedback` | `process_rates` | `reheating` from Cold to Hot; slaved source `yield` into `RateFixtureColdTracer`, slaved to `test_rate_formation.star_formation` |
| `test_rate_stripping` | `process_rates` | `stripping` from Hot to the central's Hot (`to_record: central`, zero on the central); sink `ejection` from Cold |
| `test_rate_faults` (Slice 9, extended in Slice 10) | `process_rates`, `process_full_halo` | One sink from Cold whose law a parameter selects: well behaved, a coefficient that diverges as the donor empties, NaN, negative, or impure (reads a static counter). In its full-halo mode (Slice 10) it adds mass to `RateFixtureHot` without declaring it |
| `test_rate_jump` (Slice 10) | `process_full_halo` | Merges a satellite whose Stars exceed a threshold into the central (every reservoir and tracer moved, the row retired), optionally destroying a declared fraction; `changes_totals` names both totals |

`RateFixtureCold` then has three outflows from three modules and `RateFixtureHot` three inflows from three modules, which is what lets a permutation test see a missing canonical order: two terms commute in IEEE arithmetic; three do not.

### 3.12 The proving problems

C unit tests on hand-built components with closed forms, corrected against the brief where the reference implementation showed its criterion could not be met as worded. Times are in τ units with the stated segment length Δ. Unless a row says otherwise, the absolute tolerance is 1e-3 times the relative tolerance (the defaults' ratio). The measured order p between n and 2n steps is log₂(e_n / e_2n).

| Problem | System | Criterion |
|---|---|---|
| Linear chain | A to B to C at k₁ = 1, k₂ = 0.3, Δ = 2, start (1, 0, 0); A = e^(−k₁t), B = k₁/(k₂ − k₁)·(e^(−k₁t) − e^(−k₂t)), C = 1 − A − B | Fixed steps n = 16, 32, 64, 128: \|p − 3\| ≤ 0.2 on each halving. Adaptive, Δ = 20, relative tolerance 1e-3 to 1e-10 by decades: error strictly decreasing, each per-decade ratio in [5, 20] |
| Closed cycle | Three reservoirs with nonlinear coefficients (A to B at 1 + 0.5·B, B to C at 2/(1 + C), C to A at 0.7 + 0.3·A²), Δ = 50 | Total within tol_F at 1e3, 1e4 and 1e5 fixed steps |
| Three sinks on one source | S to X, Y, Z at 0.7, 1.9, 0.31, S₀ = 1, Δ = 3 | Amounts received in the ratio of the coefficients to 1e-13 relative; with two sinks at 300 and 700 over Δ = 1 no stage or committed entry goes below zero; all six declaration orders bit-identical |
| Source, reservoir, sink | Absolute source s = 0.31416 into R, sink k = 1.73205, R₀ = 0.73681, Δ = 30; R = s/k + (R₀ − s/k)·e^(−kt), Src = s·t, Snk = s·t − (R − R₀) | At N = 1e3, 1e4 and 1e5 fixed steps: \|Src − s·Δ\| ≤ 8u·N·s·Δ and \|Snk − closed form\| ≤ \|R − R_exact\| + 8u·N·(Src + Snk); closure within tol_F |
| Stiff pair with a slow drain | A and B exchanging at kf = kb, B drained at 1, Δ = 3, started on the slow manifold (A₀ = B₀ = 0.5) | Explicit, kf = kb = 1e3 and 1e4, relative tolerance 1e-6: accepted steps between N_pos and 1.1·N_pos + 10, N_pos = ⌈Δ·K_max/(σ·C)⌉ with K_max = kb + 1. Implicit (Slice 7), kf = kb = 1e3, 1e6, 1e9, 1e12, relative tolerance 1e-3: accepted step counts equal within one; relative error at most 0.05 |
| Bimolecular | A to C at κ·B, B to C at κ·A, κ = 2, start (1, 0.6, 0), Δ = 3; with d = A₀ − B₀, B = d·B₀/(A₀·e^(κdt) − B₀), A = B + d | The chain's fixed-step order criterion against this closed form. Explicit: A − B and the total each change by at most tol_F. Implicit (Slice 7): the total within tol_F, and the A − B drift at 16, 64, 256 and 1024 fixed steps falls by a factor whose base-4 logarithm is at least 0.9 per quadrupling |
| Threshold law | G fed by a source of 1; G to S at coefficient ε·max(0, G − G_c)/G, ε = 3, G_c = 1, Δ = 2, G₀ < G_c | Fixed steps on a family whose crossing falls at a step's midpoint (G₀ chosen per n): \|p − 2\| ≤ 0.2 for every halving with n ≥ 40. Adaptive, relative tolerance 1e-4, 1e-6, 1e-8, 1e-10: error falls by a factor in [30, 300] per two decades, with at most 30 rejections per run |
| Tracer with dilution and yield (Slice 6) | Dilution: parent G₀ = 1 with pristine source 0.5, tracer Z₀ = 0.02, Δ = 3. Yield: G₀ = 1, Z₀ = 0.01, source 0.4, formation G to S at k = 1.3, yield 0.02 slaved to formation into the tracer, Δ = 3, relative tolerance 1e-8; G = s/k + (G₀ − s/k)·e^(−kt), Z = Z₀·e^(−kt) + y·(s/k)·(1 − e^(−kt)) + y·k·(G₀ − s/k)·t·e^(−kt) | Dilution concentration equals Z₀/(G₀ + 0.5·Δ) to 1e-12 relative; closed forms within 10 × the relative tolerance; tracer ledger within tol_F; yield amount equals 0.02 × formed amount within 8u·N_acc relative; an empty donor (G₀ = Z₀ = 0) gives finite values meeting the closed forms; a yield of 0.5 with no source fails with `tracer_bound` |
| Star | N = 3 and N = 30 satellites, each with gas G_s to the central's H at k_s (cross-record) and to its own stars at k′_s; H to the central's cold at k_c = 0.9; mixed Δ_s in [1, 1.5]; H(1) = H₀·e^(−b) + Σ_s Δ_s·k_s·G_s0·(e^(−a_s) − e^(−b))/(b − a_s), a_s = (k_s + k′_s)·Δ_s, b = Δ_c·k_c | At relative tolerance 1e-8, closed form within 10 × the relative tolerance; a component integrated twice, or between two others, gives a bit-identical end state and step sequence. Permutation identity is Slice 11's |
| Two intervals and a removed row | Galaxy A to B at 0.8 and satellite S to A at 1.7 (cross-record), Δ = 1, A₀ = 1, S₀ = 0.5; between segments the satellite row is removed and S merged into A; second segment A to B at 0.3; S₁ = S₀·e^(−1.7), A₁ = e^(−0.8) + 1.7·S₀·(e^(−1.7) − e^(−0.8))/(0.8 − 1.7), A₂ = (A₁ + S₁)·e^(−0.3) | At relative tolerance 1e-10, piecewise closed form within 10 × the relative tolerance; the second segment's step count and end state depend on its start state only |
| Failures | A sink at c/x with x₀ = 0.5, c = 1, Δ = 1; NaN, negative and infinite coefficients; the stiff pair at kf = kb = 1e6 with `max_steps_per_segment` 1,000 | Each returns its cause (`step_underflow`, `bad_coefficient`, `step_budget`) naming the entry and the law, and leaves the input state bitwise unchanged. A reservoir at 0.5 fed by a source of 1, with a sink whose coefficient is 10/(x − 0.5) above 0.5 and zero at or below it, Δ = 1, with `max_rejections_per_step` 5, returns `rejection_budget`: its stage coefficient grows as the step shrinks, so every retry is rejected |

---

## Repository Evidence

Precedents and anchors each slice works from, beyond those in [Frozen Facts and Evidence](#frozen-facts-and-evidence).

- **Gate helpers (Slice 1):** the snapshot-disabled battery's `pinned_schema_delta` binds `source_md5` to each side's digest (`tests/manual/test_snapshot_disabled_identity.py:342-370`); its hook `metadata_delta()` judges a differing `output_schema.json` itself and errors unless the pinned delta holds (`:380-397`), and its comparator self-test requires "source_md5 changed alone" to be an error (`:1010`), run by `make tests-snapshot-global-identity`. The reference gate fills each `OutputRun.source_md5` from its worktree's `build/generated/property_hash.txt` (`tests/manual/test_reference_identity.py:434-443`), passes no hook (`:497-499`) and logs every permitted difference by label (`:502-506`). Besides these two gates, only `tests/integration/test_parity_gate_helpers.py` calls `compare_outputs()`. `tests/integration/test_parity_gate_helpers.py` tests the comparison helpers and needs `TMPDIR` outside the repository. The gate's permitted differences are described in `tests/README.md` (the manual-tests paragraph), the Developer Guide's Testing section and the validation skill's manual-gates row.
- **Generator tests (Slices 2 and 4):** `tests/integration/test_unit_contract_generation.py` imports generator functions in-process, asserts emitted text and `ValueError` messages, and compiles one emitted program; `tests/integration/test_event_schema_validation.py` is the precedent for module-metadata rules tested on synthetic dictionaries; the X-macro precedent for a generated table is `catalog_field_metadata.inc`.
- **Mode addition (Slice 3):** appending `PROCESSING_MODE_RATES` after `PROCESSING_MODE_SNAPSHOT` keeps every existing enumerator's value (`tests/unit/test_snapshot_module_contract.c` pins the first three); `test_fixtures_register_only_in_test_builds` in `tests/integration/test_snapshot_module_schema.py` matches adjacent `.process` and `.process_snapshot` lines of generated text, which a member emitted after `.process_snapshot` leaves intact; `test_fof_phase_rejects_snapshot_family` in `tests/unit/test_snapshot_module_contract.c` is the precedent for a phase refusing a family.
- **Fixtures (Slices 4 and 8 to 10):** `src/module_system/test_snapshot_fixture/` is the precedent for a fixture that adds a mode; fixture `.c` files reach the unit link through the generated `tests/generated/module_sources.txt`; `tests/framework/test_phase_config.h` builds phase configurations for C tests; `tests/unit/test_output_buffer.c` builds a workspace over test-owned rows; `expect_fatal()` in `tests/framework/child_capture.h` runs a body in a child process and requires it to abort with named message fragments.
- **Run-file keys (Slices 8 and 9):** the `simulation.cosmology` nested mapping uses `reject_unknown_keys()` (`src/core/read_parameter_file.c:1044`); `parse_timestep_scheme()` is the precedent for a scheme string; the checklist in `.agents/skills/mimic-config-and-flags/SKILL.md` ("Adding a configuration axis") lists the touchpoints.
- **Provenance (Slice 10):** `write_event_contracts()` (count pass, early return, compound rows) is the template for `RateTransfers`; `tests/integration/test_output_formats.py` reads `RunProperties` attributes by name.
- **Batteries (Slice 11):** `tests/manual/run_snapshot_global_battery.py` (own pairs, case counts, SKIP and NA as failures, restore in `finally`) and `tests/manual/test_distributed_identity.py` (`mpirun -np 1, 2, 3, 4, 8`, `forest_chunks` 2, 3, 8, `--compare-created`, per-partition row order) are the models; `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_halos-only.yaml` is the committed fixture run file. In CI, the `horizontal-v3` job (`.github/workflows/ci.yml:97`) runs `make tests-distributed` in a step that sets `MPIRUN: "mpirun --oversubscribe"` on itself (`:160-164`).
- **Cost (Slice 12):** `scripts/profiling/run_wall.py <run.yaml> <repeats> <label> -o <out.json>` times serial runs and records peak child RSS; every run prints `Peak process RSS` at exit. `tests/manual/test_distributed_identity.py` compiles its MPI control test `tests/mpi/test_snapshot_collectives_mpi.c` itself against an explicit source list (`CONTROL_SOURCES`, `:136-138`; the compile command, `:289-292`), with no `Makefile` rule of its own.

---

## Execution Structure, Implementation Profiles and Run Preparation

**Mode B, one branch.** `feature/coupled-rate-mode`, created from `main` at the plan commit. Slices run in order, one at a time; no batches are declared, and each slice is independently gateable. The Design above is the specification each slice's Intended Change cites; the Developer reads it.

**The standing checks (every slice; slices refer to them by this name).** `./scripts/beautify.sh`; `make check-format`; `make check-docs`; `make generate && make check-generated` on the default pair in production mode, never under `TEST_BUILD=yes`; `make tests summary` on the default pair, delegated to a subagent with the log captured under `archive/test-logs/` and the exit code checked. Never run two suites at once. After any battery or tier that builds another pair, restore the default pair with `make generate && make`.

**The converter suite.** A slice that changes a file under `src/`, `scripts/`, `tests/framework/`, `tests/unit/tools/` or the `Makefile` also runs `make tests-converter` (about 6 min, delegated, exit code recorded).

**The bench suite.** Slices 2, 8, 9 and 10 run `make tests-bench` (about 5 min), because they touch the output schema writer, run-file keys or `RunProperties`.

**The byte gate.** `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` for every slice that changes `src/`, `scripts/`, `models/`, `simulations/` or the `Makefile`, and wherever a criterion names it. It refuses uncommitted runtime edits, so it runs on the slice's committed state; a failure is fixed by a further commit, never an amend. From Slice 2 on, each leg must report Slice 1's schema-provenance label as its only permitted schema difference. It needs the real mini-Millennium data under `/Volumes/Internal/data/millennium/` in both layouts and fails, never skips, without it.

**Standing rules (bind every slice).** Nothing outside `docs/dev/` may reference a `docs/dev/` document, including guides, READMEs, `CHANGELOG.md`, skills, code comments and tests. No slice edits the four shipped run files the byte gate runs (`models/{halos-only,sage16}/input/*_mini-millennium{,-horizontal}.yaml`). No slice regenerates a baseline; a baseline that will not pass is a stop. Suites run with the default `TMPDIR`: a `TMPDIR` inside the repository fails two parity-gate helper tests. `cp` and `mv` are aliased to `-i`; use `\cp` and `\mv`. Documentation lands as decision 11 says.

**Developer seats** (`Recommended Developer:` per slice; full model ids for the PM). Risk is what `check-plan` derives from the flags.

| Slice | Developer | Effort | Difficulty | Risk | Reason |
|---|---|---|---|---|---|
| 1 | `claude-sonnet-5-5` | high | moderate | elevated | Tightly specified standing rule in the gates' shared comparison |
| 2 | `claude-opus-5-5` | high | hard | elevated | Generator edit reaching every package and every test-build schema |
| 3 | `claude-sonnet-5-5` | high | moderate | elevated | Mechanical extension of a closed table, ABI addition |
| 4 | `claude-opus-5-5` | high | hard | elevated | Two validators and a generator in lock step, cross-module resolution, four fixtures |
| 5 | `claude-opus-5-5` | xhigh | hard | elevated | New numerics where a subtle error is costly |
| 6 | `claude-opus-5-5` | high | hard | elevated | Tracer and slaved-source numerics on the Slice 5 unit |
| 7 | `claude-opus-5-5` | xhigh | hard | elevated | Subtraction-free block solve and its error estimate |
| 8 | `claude-opus-5-5` | high | hard | elevated | Core dispatch, assembly, run-file keys, global state |
| 9 | `claude-sonnet-5-5` | high | moderate | elevated | Failure file, purity probe and a fault fixture on Slice 8's dispatch |
| 10 | `claude-opus-5-5` | high | hard | elevated | HDF5 provenance and an MPI reduction |
| 11 | `claude-sonnet-5-5` | high | moderate | elevated | Battery on established patterns; Makefile and CI |
| 12 | `claude-sonnet-5-5` | high | moderate | standard | Runs gates and measures; writes the record |
| 13 | `claude-sonnet-5-5` | high | moderate | standard | Documentation from frozen text and an inventory |

**Reviewers.** The drift auditor and code reviewers are the panel the launcher names; the slices marked `Independent audit required: yes` (1 to 11) must have both reviews fresh at the final commit.

**After the run (the owner's steps, not a slice).** Merge; move this plan and the acceptance record to `archive/dev-plans/` with the pathway's links repaired; release by the backlog's recipe ([`MIMIC-BACKLOG.md`](MIMIC-BACKLOG.md)).

---

## Resource Profile

Plan the whole run at 26 to 40 hours.

| Slice | Developer wall time | Machine time for validation | Data |
|---|---|---|---|
| 1 | hours | byte gate (minutes cold, about a minute warm), snapshot-disabled gate (about a minute), converter suite (6 min), suite (5 min) | real mini-Millennium, both layouts |
| 2 | a day | three generate-and-check pairs, sage26 scientific tier (1 min), `tests-snapshot-global`, `tests-horizontal-v3`, byte gate, bench (5 min), converter, suite | real mini-Millennium |
| 3 | hours | snapshot-global battery (under a minute warm), byte gate, converter, suite | real mini-Millennium |
| 4 | a day | test and production builds, byte gate, converter, suite | real mini-Millennium |
| 5, 6, 7 | a day each | unit tests (seconds to minutes), byte gate, converter, suite | none beyond the gate's |
| 8 | a day | byte gate, `tests-snapshot-global`, `tests-horizontal-v3`, `tests-distributed` (minutes each), bench, converter, suite | real mini-Millennium; `forest_blocks` |
| 9 | hours | the rate-phase integration test in a test build, byte gate, bench, converter, suite | real mini-Millennium |
| 10 | a day | as Slice 8, plus `tests-snapshot-global-identity` and an `mpirun -np 2` run | as Slice 8 |
| 11 | hours to a day | `make tests-rate` (tens of minutes cold), the three batteries, byte gate, converter, suite | real mini-Millennium, both layouts; `forest_blocks` |
| 12 | a day of attention | five `make tests` runs (about 25 min), 50 generate-and-check pairs (about 5 min), every gate (about an hour), cost runs (under an hour) | real mini-Millennium |
| 13 | hours | standing checks, byte gate, converter | none |

---

## Slice 1: The identity gates treat the schema's generator hash as build provenance

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Frozen Facts on gates and suites, its Repository Evidence on gate helpers, its decisions 4 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- In `tests/framework/parity_gate.py`, `compare_metadata_file()` gains a standing rule for `metadata/output_schema.json`, beside its existing rule for `metadata/version_info.json` and after any `metadata_delta` hook has had first refusal (the function already calls the hook ahead of its identical-bytes and path-prefix rules, `:1609-1617`): two schemas that differ are accepted, and recorded under one fixed label, when their parsed contents are equal apart from `source_md5` and each side's `source_md5` equals that side's build digest (`OutputRun.source_md5`); any other difference is an error, and a side with no known digest is an error. The rule is not keyed to any commit and has no expiry.
- The rule applies to every caller of `compare_outputs()`. The snapshot-disabled battery keeps its stricter verdict because its own hook judges that file first (`metadata_delta()` in `tests/manual/test_snapshot_disabled_identity.py:380-397` errors on a `source_md5` change alone).
- The reasoning, stated once in the code: `source_md5` is a digest of the generator and its inputs, not of the schema, which is how `simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py` already treats it (`SCHEMA_PROVENANCE_KEYS`, `:164`).
- `tests/manual/test_reference_identity.py` does not change: it already fills `OutputRun.source_md5` from each worktree's `build/generated/property_hash.txt` (`:434-443`), passes no hook (`:497-499`) and logs every permitted difference by label (`:502-506`).
- Document the rule where the gates' permitted differences are described today: `tests/README.md` (the manual-tests paragraph), the Developer Guide's Testing section and the validation skill's manual-gates row.

### Acceptance Criteria
- [ ] Under the standing rule, a schema pair identical except for `source_md5`, each value equal to its side's build digest, is accepted and recorded in the report's permitted differences under one fixed label.
- [ ] Under the standing rule, a pair whose `source_md5` on either side differs from that side's build digest is an error.
- [ ] Under the standing rule, a pair that differs in any other key or value (a field added, removed or reordered, a description, a unit, `binary_record_size`) is an error, with or without a `source_md5` difference.
- [ ] Under the standing rule, a side whose build digest is unknown is an error, never accepted.
- [ ] Each of the four cases above is a case in `tests/integration/test_parity_gate_helpers.py`, table-driven where the fixture is shared.
- [ ] A caller that passes no hook gets the rule: shown by a helper test that calls `compare_outputs()` with no `metadata_delta`.
- [ ] The snapshot-disabled battery's self-test, which requires a `source_md5` change alone to be an error under its pinned hook (`tests/manual/test_snapshot_disabled_identity.py:1010`), still passes.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` exits 0 and no leg reports the rule's label.
- [ ] `make tests-snapshot-global-identity` exits 0.
- [ ] `tests/README.md`, the Developer Guide's Testing section and the validation skill's manual-gates row each describe the standing rule.
- Behaviour that must not change: every other comparison the gates make; the snapshot-disabled battery's verdicts.

### Authorized Surface
- Files allowed to change:
  - `tests/framework/parity_gate.py`
  - `tests/integration/test_parity_gate_helpers.py`
  - `tests/README.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `docs/DEVELOPER-GUIDE.md`
- Functions/classes/components allowed to change: `compare_metadata_file()` and one schema-judging helper beside it in `parity_gate.py`; the manual-tests paragraph of `tests/README.md`; the Developer Guide's Testing section only; the validation skill's reference-identity row.
- Tests allowed or expected to change: new cases in `tests/integration/test_parity_gate_helpers.py`.

### Explicit Non-Goals
- No permitted difference for an attribute or object present on one side only; no change to `tests/manual/test_reference_identity.py`, `tests/manual/test_snapshot_disabled_identity.py`, `scripts/compare_cross_format_identity.py` or `scripts/generate_properties.py`; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: the identity gates' shared comparison
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the four cases in `tests/integration/test_parity_gate_helpers.py`.
- Commands to run: the standing checks; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice; `make tests-snapshot-global-identity`; `python3 tests/integration/test_parity_gate_helpers.py` with the default `TMPDIR`.
- Lint (differential, via the lint skill): required.
- Manual checks: read `build/reference_identity.log` and confirm eight PASS legs on the real data and no line carrying the rule's label.

### Rollback Path
- Revert the slice's commits; the gates return to byte comparison of `output_schema.json`, and before Slice 2 nothing depends on the rule.

## Slice 2: Reservoir metadata and the generated reservoir table

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.1 and 3.11 (the fixture properties), its decisions 4, 8, 11 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement Design 3.1 in `scripts/generate_properties.py`: the `reservoir:` validation and a new generated header under `src/include/generated/` (suggested name `reservoir_table.h`), added to `scripts/check_generated.py`'s list.
- Add the seven fixture properties of Design 3.11 to `src/module_system/test_fixture/test_properties.yaml`.
- Change the two SAGE physics-baseline tests as decision 8 says, through one shared helper in `tests/framework/harness.py` that returns the test-fixture output field names, read from `test_properties.yaml` in declaration order.
- Document the `reservoir:` key in the Developer Guide's Property Metadata Schema table and in `.agents/skills/mimic-properties/SKILL.md`.

### Acceptance Criteria
- [ ] Each refusal of Design 3.1 (unknown key in the mapping; the mapping on a halo or core property; a type other than scalar `double`; `init_repeat` on a reservoir or tracer; a tracer whose parent is missing, is not a reservoir, or shares its total; two tracers of one parent in the same total; a total that holds both reservoirs and tracers; two members of one total whose `units` strings differ) is raised with a message naming the property, checked in a table-driven Python test that calls the generator's validation in-process.
- [ ] A property set with no `reservoir:` mapping generates a reservoir table with a count of zero that compiles clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`.
- [ ] The production builds of the default pair and of `MODEL=sage26 SIMULATION=mini-millennium` are warning-free.
- [ ] In a test build the table has exactly the seven fixture entries in YAML order, and a C unit test asserts each entry's name, kind, parent index and total index, that its offset equals `offsetof(struct GalaxyData, <field>)`, and that the two totals are named `rate_fixture_mass` and `rate_fixture_tracer`.
- [ ] The new generated file is in `scripts/check_generated.py`'s list, and `make generate && make check-generated` passes on the default pair, on `MODEL=sage26 SIMULATION=mini-millennium` and on `MODEL=halos-only SIMULATION=mini-millennium-horizontal`.
- [ ] In a test build the seven fixture properties are written to binary and HDF5 output with value 0 on a pipeline that does not touch them, and the core range and zero-value checks report no failure for them.
- [ ] The two SAGE physics-baseline tests pass in a test build, asserting that the output field names equal the baseline's names followed by the fixture output names in schema order.
- [ ] Each of those tests still fails if a production field is added, removed or renamed, shown by a case that feeds the comparison a doctored name list.
- [ ] `make MODEL=sage26 SIMULATION=mini-millennium tests-scientific` passes with `MIMIC_BASELINE_RTOL` unset.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` exits 0, every leg reports Slice 1's schema-provenance label and no other schema difference, and every galaxy byte is identical.
- [ ] `make tests-snapshot-global` and `make tests-horizontal-v3` each exit 0.
- [ ] No file under `models/` other than the two named test files changes, and no baseline file changes.
- Behaviour that must not change: production output, apart from `source_md5` in `metadata/output_schema.json`.

### Authorized Surface
- Files allowed to change:
  - `scripts/generate_properties.py`
  - `scripts/check_generated.py`
  - `src/module_system/test_fixture/test_properties.yaml`
  - `tests/unit/test_reservoir_table.c` (new file)
  - `tests/integration/test_reservoir_metadata.py` (new file)
  - `tests/framework/harness.py`
  - `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`
  - `models/sage26/modules/_tests/test_scientific_sage26_physics_baseline.py`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-properties/SKILL.md`
- Functions/classes/components allowed to change: property validation and one new emitter in the generator; `PROPERTY_GENERATED_FILES`; the fixture property file; one new harness helper; the field-name assertions of the two baseline tests; the Property Metadata Schema table; the properties skill's schema table and generated-file list.
- Tests allowed or expected to change: the two new test files; the two baseline tests.

### Explicit Non-Goals
- No reservoir declared in any model or simulation package; no use of the table at run time; `scripts/validate_modules.py` and `scripts/generate_module_registry.py` are untouched (Slice 4); and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: generated code for every package; the output schema of every test build; shared generated types
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/integration/test_reservoir_metadata.py` (refusal table, the empty-table compile, the output of a test-build run); `tests/unit/test_reservoir_table.c`; the doctored-name case in each baseline test.
- Commands to run: the standing checks; `make generate && make check-generated` for the three named pairs, then the default pair restored; `make MODEL=sage26 SIMULATION=mini-millennium tests-scientific` with `MIMIC_BASELINE_RTOL` unset; `make tests-snapshot-global` and `make tests-horizontal-v3` (both test builds whose output schema this slice changes), then the default pair restored; `make tests-bench`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: in a test-build run's `metadata/output_schema.json`, the seven fixture fields come last, with units and descriptions.

### Rollback Path
- Revert the slice's commits; generation, test-build schemas and the two baseline tests return to the baseline, and Slice 1's rule stays unused.

## Slice 3: The rate mode and its callback family

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Frozen Facts on mode machinery, its Design section 3.3, its decisions 2, 11, 17 and 19, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Add `PROCESSING_MODE_RATES` with string `process_rates` and family `MODULE_CALLBACK_FAMILY_RATE` to the C table and to `scripts/module_modes.py` (family key `rate`, field `rates`, symbol suffix `_rates`).
- Add the `rates` member of `struct Module`, `int (*rates)(const struct RateContext *ctx, const struct Halo *halo, double *coefficients)`, and declare `struct RateContext` with the fields of Design 3.3: the snapshot number and redshift, the row's own time at the evaluation point, a `const` pointer to the scratch copy of the FoF central at the trial state, documented as NULL unless the module declares `reads_central: true` (decision 19), and the read-only configuration, and no solver step, substep index or substep count.
- Add a new running-callback kind for rate callbacks (suggested `RUNNING_CALLBACK_RATE`), handled by both `switch` statements and by `check_module_callback_families()`.
- Make `build_phase_plan_visitor()` fail fatally when its lists do not cover every entry of the phase. The mode cannot run yet: every phase still refuses it at startup.

### Acceptance Criteria
- [ ] The C mode table, its count assertion and `scripts/module_modes.py` agree, proved by the existing mirror test extended to the new family and field.
- [ ] Registering a module that advertises `process_rates` with a NULL `rates` callback is fatal, and one with the callback registers, shown by a C unit test with hand-built modules.
- [ ] A configuration naming `process_rates` in `pre_timestep`, in a named substep phase, in `post_timestep` or in `post_snapshot` is refused at startup with a message naming the phase and the module, before any `init()` runs.
- [ ] A standalone `.c` module cannot advertise `process_rates` (generator test).
- [ ] `build_phase_plan_visitor()` checks that its lists cover every entry of the phase and fails fatally otherwise. No test seam is added: it is an internal invariant that no valid configuration reaches, verified by reading the code.
- [ ] Both `switch` statements over the running-callback kind handle the new kind, and the project builds warning-free.
- [ ] `process(ctx, halos, ngal)` and `process_snapshot()` keep their signatures.
- [ ] Every existing module's generated registration differs from the baseline's only by the added NULL `rates` member and the source-hash header line, on the default pair and on `MODEL=sage26 SIMULATION=mini-millennium`, and `make generate && make check-generated` passes on both.
- [ ] `make tests-snapshot-global` exits 0.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` exits 0.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_interface.h`
  - `src/core/module_registry.h`
  - `src/core/module_registry.c`
  - `src/core/processing_modes.c`
  - `src/core/snapshot_collectives.c`
  - `src/core/read_parameter_file.c`
  - `scripts/module_modes.py`
  - `scripts/generate_module_registry.py`
  - `scripts/validate_modules.py`
  - `tests/unit/test_snapshot_module_contract.c`
  - `tests/unit/test_module_configuration.c`
  - `tests/unit/test_rate_mode_contract.c` (new file)
  - `tests/integration/test_snapshot_module_schema.py`
- Functions/classes/components allowed to change: the mode and family enumerations, `struct Module`, `struct RateContext`, `enum RunningCallbackKind`, the descriptor table, `check_module_callback_families()`, `build_phase_plan_visitor()`, `running_callback_description()`, `callback_kind_phrase()`; in `read_parameter_file.c` only comments that list phase legality; the Python descriptors; generator and validator comments and legends.
- Tests allowed or expected to change: the mirror and generation tests in `test_snapshot_module_schema.py`; the mode tables in `test_snapshot_module_contract.c`; `test_module_configuration.c`; the new contract test.

### Explicit Non-Goals
- No dispatch of the mode; no run-file key; no `rates:` metadata; no change to `module_emit_event()` (Slice 8); and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: the module ABI struct; the core module registry; generated registration for every package
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_rate_mode_contract.c`; the mirror, standalone and registration cases in `tests/integration/test_snapshot_module_schema.py`; the expected-family table in `tests/unit/test_snapshot_module_contract.c`.
- Commands to run: the standing checks; `make MODEL=sage26 SIMULATION=mini-millennium generate check-generated`, then the default pair restored; a diff of each pair's generated `module_init.c` against the baseline's; `make tests-snapshot-global`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: read `build_phase_plan_visitor()` and confirm it checks that its lists cover every entry of the phase and fails fatally otherwise.

### Rollback Path
- Revert the slice's commits; the mode table, ABI and generated registration return to the baseline.

## Slice 4: Transfer declarations and the rate fixture modules

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.2 and 3.11, its decisions 2, 6, 7, 10, 11, 17 and 19, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement Design 3.2 in `scripts/generate_module_registry.py` and `scripts/validate_modules.py`, with shared rules in `scripts/module_modes.py` where both tools need them: the `rates:` validation (including `reads_central`, the same-total rule for internal transfers and the refusal of a dependency cycle among totals over all compiled rate modules), the global transfer index (decision 10), the generated per-module transfer and observer tables with reservoir indices and the module's `reads_central` flag, the coefficient index constants and the `rates` binding.
- Add the transfer and observer descriptor types and their `struct Module` members, including the `reads_central` flag, to `src/core/module_interface.h`.
- Add the four rate fixture modules of Design 3.11 (`test_rate_supply`, `test_rate_formation`, `test_rate_feedback`, `test_rate_stripping`) with their laws, parameters, READMEs and `Makefile` source lines.
- Document the `rates:` keys in the Developer Guide's Module Metadata Schema, in `.agents/skills/mimic-modules/SKILL.md` and in `src/module_system/README.md`'s fixture list.

### Acceptance Criteria
- [ ] Each refusal of Design 3.2 that can be decided from metadata (both ends `external`; `reads_central` not a boolean; an end that is not a declared reservoir, with a tracer allowed as `to` only from `external`; an internal transfer whose two reservoirs belong to different conserved totals; `to_record: central` with `to: external`; a duplicate transfer name; a `slaved_to` naming anything but a transfer with a reservoir donor, or one whose donor's total equals the recipient's; two totals each fed by a source slaved to a sink of the other, refused with a message naming the totals; an observer naming a property not of kind `observer` or an undeclared transfer; `rates:` without `process_rates` or the reverse) is raised by the generator and by `validate_modules.py` with a message naming the module and the transfer, in a table-driven in-process test.
- [ ] Each tool rejects an unknown key inside `rates:`.
- [ ] For the four fixtures the generated tables carry every transfer and observer with the reservoir indices of Slice 2's table, the slaved master resolved across modules, one coefficient index constant per transfer, and a `reads_central` flag that is false for each (none of the four declares it), checked by a C unit test against the fixture declarations. The test asserts per-module tables and the relative order of the global transfer index (module name, then declaration order), never absolute index values, so a fixture added by a later slice does not break it.
- [ ] The global transfer index orders transfers by module name then declaration order, and is unchanged when the fixture modules are discovered in a different order (generator test with the discovery order reversed).
- [ ] Removing a property a transfer names fails at generation.
- [ ] The emitted transfer tables name reservoirs by the generated index symbols, never by literal integers, shown by a generator test on the emitted text, so a table left stale by a removed reservoir fails to compile.
- [ ] A test build compiles and registers the four fixtures with the project warnings clean.
- [ ] With `MIMIC_TEST_BUILD=1` set, `make validate-modules` and `make lint-parameters` pass.
- [ ] Each fixture law, called directly in a C unit test with a hand-built row, returns the coefficients its README states for a table of inputs, including zero for `stripping` on a Type 0 row and zero for `star_formation` below its threshold, with the expected values written independently in the test.
- [ ] A production build contains no fixture module and its generated registration has no rate table.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` exits 0.

### Authorized Surface
- Files allowed to change:
  - `scripts/generate_module_registry.py`
  - `scripts/validate_modules.py`
  - `scripts/module_modes.py`
  - `scripts/check_generated.py`
  - `Makefile`
  - `src/core/module_interface.h`
  - `src/module_system/test_rate_supply/` (new directory)
  - `src/module_system/test_rate_formation/` (new directory)
  - `src/module_system/test_rate_feedback/` (new directory)
  - `src/module_system/test_rate_stripping/` (new directory)
  - `src/module_system/test_fixture/test_properties.yaml`
  - `src/module_system/README.md`
  - `tests/unit/test_rate_transfer_tables.c` (new file)
  - `tests/unit/test_rate_mode_contract.c`
  - `tests/integration/test_rate_metadata.py` (new file)
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-modules/SKILL.md`
- Functions/classes/components allowed to change: metadata loading, validation and emission for `rates:` in the two tools and the shared helpers; `check_generated.py` only if a hashed input or generated file is added; the `Makefile` test-build fixture source lines only; the transfer and observer descriptor types and `struct Module` members; `test_properties.yaml` only if a fixture law needs a property Slice 2 did not declare; the Module Metadata Schema section; the modules skill's schema table; the fixture list in `src/module_system/README.md`.
- Tests allowed or expected to change: the three named test files.

### Explicit Non-Goals
- No integration or dispatch; no `changes_totals` (Slice 10); no faults or jump fixture (Slices 9 and 10); and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: the metadata generators and validator; the Makefile test-build source list
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/integration/test_rate_metadata.py` (refusals, unknown keys, ordering, stale-symbol text); `tests/unit/test_rate_transfer_tables.c` (tables and laws); `tests/unit/test_rate_mode_contract.c` if registration cases extend.
- Commands to run: the standing checks; `make TEST_BUILD=yes` on the default pair; `MIMIC_TEST_BUILD=1 make validate-modules`; `MIMIC_TEST_BUILD=1 make lint-parameters`; a production `make generate && make` with the generated `module_init.c` inspected for fixtures and rate tables; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice. Record in `validation.md` whether `test_properties.yaml` changed and why.
- Lint (differential, via the lint skill): required.
- Manual checks: read each fixture README against its law and its unit-test table.

### Rollback Path
- Revert the slice's commits; the fixtures, descriptor types and `rates:` handling disappear, and Slices 2 and 3 stand as they were.

## Slice 5: The explicit integrator and the segment ledger

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.4, 3.5, 3.7 (the segment closure) and 3.12, its Measured numerics, its decisions 3, 5, 10, 12 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Add a pure-numerics unit in `src/core/` (suggested `rate_integrator.c` and `rate_integrator.h`) that advances one component across one segment: Design 3.4 (normalised time, per-row Δ), Design 3.5 (scheme, positivity, error control, controller, first step, landing, defaults, constants, failure record) and the segment closure of Design 3.7, for internal transfers, sources and sinks.
- Its interface takes a description of the component (entries, their totals, the fluxes with their kinds, ends and global transfer indices, per-row Δ), a callback that fills the coefficient array for a given state, the tolerances and budgets, and returns the advanced state, a status with its failure record, and the segment's step statistics. It also takes a fixed number of equal steps with the controller off, for the order tests. The segment closure check is a function of a start state, an end state and the external amounts, callable on its own. It knows nothing of galaxies, modules, the workspace or the run file, and takes its memory from a caller-owned workspace.
- Frozen numerical contract, for review: tableau c = (0, 1/2, 1, 1/2), A rows (1/2), (1/2, 1/2), (1/6, 1/6, 1/6), b = (1/6, 1/6, 1/6, 1/2), b̂ = (1/4, 1/4, 1/4, 1/4); one amount per flux applied to both ends as `(x − Σ outflows) + Σ inflows` in canonical order; C = 2, σ = 0.9: cap the step at σ·C/K_max before the first stage, reject when step × K_max at any stage exceeds C, reject when any stage or final entry is strictly below zero, and after a positivity rejection take the smaller of half the step and σ·C over the largest K_max seen; K_max everywhere (cap, stage check, retry, statistics) is over entries with non-zero content at the state in question, and with no such entry the cap is +∞; no law evaluated at a negative state and nothing clamped; weight `absolute_tolerance` × S + `relative_tolerance` × max(|x| start, |x| trial), S floored at 1e-12 of the component total; max norm, a ratio with a zero numerator is zero (so a zero error contributes 0), a zero weight under a non-zero error or a NaN +∞; factor 0.9·err^(−1/3) clamped to [0.2, 5], no growth after a rejection; the first-step and landing rules of Design 3.5, in which an entry with zero weight w0_i is left out of d0, d1 and d2 and, when every entry is left out, dt0 = min(1, cap) with no probe and no extra evaluation; landing stretch 10%; minimum step 2⁻⁴⁶; defaults 1e-4, 1e-7, 100000 attempts, 30 consecutive rejections; causes `bad_coefficient`, `step_budget`, `rejection_budget` (`max_rejections_per_step` consecutive rejections of one step), `step_underflow`, `ledger`; a row with Δ = 0 donates nothing and its own laws and sources move nothing, but it receives what a donor with positive Δ sends it.
- Terms the criteria use: u = 2⁻⁵³; L is the largest number of inflow plus outflow terms on one entry; N_acc the accepted steps; n_F the entries of total F; tol_F = 8u·[(L + 2)·N_acc + n_F + 2]·(Σ_F|x(start)| + Src_F + Snk_F); K_i is Δ of the entry's row times the sum of coefficients out of entry i; K_max is the largest K_i over entries with non-zero content; N_pos = ⌈Δ·K_max/(σ·C)⌉; p = log₂(e_n/e_2n); the error of a test problem is the largest absolute difference between a computed entry and its closed form at τ = 1; unless stated, the absolute tolerance is 1e-3 times the relative tolerance.

### Acceptance Criteria
- [ ] Linear chain, A to B to C at k₁ = 1, k₂ = 0.3, Δ = 2, start (1, 0, 0): at fixed n = 16, 32, 64 and 128 steps, |p − 3| ≤ 0.2 on each halving against A = e^(−k₁t), B = k₁/(k₂ − k₁)·(e^(−k₁t) − e^(−k₂t)), C = 1 − A − B.
- [ ] Linear chain adaptive, Δ = 20, relative tolerance 1e-3 to 1e-10 by decades: the end error is strictly decreasing and each per-decade ratio lies in [5, 20].
- [ ] Closed cycle of three reservoirs with nonlinear coefficients (A to B at 1 + 0.5·B, B to C at 2/(1 + C), C to A at 0.7 + 0.3·A²), Δ = 50: the total is within tol_F after 1e3, 1e4 and 1e5 fixed steps.
- [ ] Three sinks S to X, Y, Z at 0.7, 1.9 and 0.31, S₀ = 1, Δ = 3: the amounts received are in the ratio of the coefficients to 1e-13 relative.
- [ ] Two sinks at 300 and 700 over Δ = 1: no stage or committed entry goes below zero.
- [ ] The three sinks listed in all six orders, each keeping its global transfer index, give bit-identical end states.
- [ ] Source s = 0.31416 into R, sink k = 1.73205, R₀ = 0.73681, Δ = 30, at N = 1e3, 1e4 and 1e5 fixed steps: |Src − s·Δ| ≤ 8u·N·s·Δ, |Snk − (s·Δ − (R_exact − R₀))| ≤ |R − R_exact| + 8u·N·(Src + Snk), and closure within tol_F, with R_exact = s/k + (R₀ − s/k)·e^(−kΔ).
- [ ] Stiff pair, A and B exchanging at kf = kb, B drained at 1, Δ = 3, A₀ = B₀ = 0.5, relative tolerance 1e-6: for kf = kb = 1e3 and for 1e4, accepted steps lie between N_pos and 1.1·N_pos + 10 with K_max = kb + 1.
- [ ] Bimolecular, A to C at κ·B and B to C at κ·A, κ = 2, start (1, 0.6, 0), Δ = 3, with d = A₀ − B₀, B = d·B₀/(A₀·e^(κdt) − B₀), A = B + d: at fixed n = 16, 32, 64 and 128 steps |p − 3| ≤ 0.2 on each halving against this closed form, and at every n, A − B and the total each change by at most tol_F.
- [ ] Threshold law, G fed by a source of 1 and G to S at coefficient 3·max(0, G − 1)/G, Δ = 2: with G₀ chosen per n so that the crossing falls at a step's midpoint, |p − 2| ≤ 0.2 for every halving with n ≥ 40.
- [ ] Threshold law adaptive at relative tolerance 1e-4, 1e-6, 1e-8 and 1e-10: the error falls by a factor in [30, 300] per two decades, with at most 30 rejections per run.
- [ ] Two intervals, galaxy A to B at 0.8 and satellite S to A at 1.7 (cross-record), Δ = 1, A₀ = 1, S₀ = 0.5, then the satellite row removed and S merged into A, then A to B at 0.3: at relative tolerance 1e-10 the piecewise closed form (S₁ = S₀·e^(−1.7), A₁ = e^(−0.8) + 1.7·S₀·(e^(−1.7) − e^(−0.8))/(0.8 − 1.7), A₂ = (A₁ + S₁)·e^(−0.3)) holds within 10 × the relative tolerance.
- [ ] The second interval's step count and end state equal those of the same segment integrated from an identical, freshly built start state.
- [ ] Star of N = 3 and N = 30 satellites (gas G_s to the central's H at k_s, to its own stars at k′_s; H to the central's cold at 0.9; Δ_s in [1, 1.5]): at relative tolerance 1e-8, H(1) is within 10 × the relative tolerance of H₀·e^(−b) + Σ_s Δ_s·k_s·G_s0·(e^(−a_s) − e^(−b))/(b − a_s), a_s = (k_s + k′_s)·Δ_s, b = Δ_c·0.9.
- [ ] A sink at c/x with x₀ = 0.5, c = 1, Δ = 1 fails with `step_underflow`; NaN, negative and infinite coefficients each fail with `bad_coefficient`; the stiff pair at kf = kb = 1e6 with `max_steps_per_segment` 1,000 fails with `step_budget`.
- [ ] A reservoir at 0.5 fed by a source of 1, with a sink whose coefficient is 10/(x − 0.5) above 0.5 and zero at or below it, Δ = 1, with `max_rejections_per_step` 5, fails with `rejection_budget`.
- [ ] A sink out of A at coefficient 1 + 50·B fed by a source of 10 into B, A₀ = 1, B₀ = 0, Δ = 2, is integrated with no stage or committed entry below zero.
- [ ] With rows of different Δ, a cross-record transfer conserves within tol_F.
- [ ] A row with Δ = 0 donates nothing and receives no source; fed by a donor row with positive Δ it gains exactly the donor's amount, and the total closes within tol_F; a segment whose every Δ is zero takes no step and evaluates no law.
- [ ] An all-empty component with no source and non-zero coefficients takes exactly one accepted step, and every entry stays exactly zero.
- [ ] An initially empty reservoir fed by an absolute source s = 0.4 with a sink at k = 1.3, Δ = 3, at relative tolerance 1e-8, meets R = (s/k)·(1 − e^(−kΔ)) within 10 × the relative tolerance, with every step and weight finite.
- [ ] The closure check, called on a hand-built start and end state whose total differs by a relative 1e-6 with no external amount, returns cause `ledger`, and returns success when the difference is the external amount.
- [ ] The same star component, state and tolerances give a bit-identical end state and step sequence on repeated calls, and after integrating a different component in between.
- [ ] Every failure leaves the caller's state bitwise unchanged, and its record names the cause, the entry and the flux.
- [ ] The unit links standalone in `tests/unit/run_tests.sh` and adds no dependency to the files `tests/unit/tools/build_topology_dump.sh` links, and `make tests-converter` passes.
- [ ] Expected values in every test come from closed forms written in the test, never from the integrator's own helpers.
- [ ] Every test system's values are literals in the test; random components come from a fixed-seed generator written in the test.
- Inputs: coefficients finite and non-negative; entries non-negative at segment start; Δ ≥ 0; tolerances in (0, 1). Behaviour outside this domain is unspecified, except where a criterion above requires a named failure.

### Authorized Surface
- Files allowed to change:
  - `src/core/rate_integrator.c` (new file)
  - `src/core/rate_integrator.h` (new file)
  - `tests/unit/test_rate_integrator.c` (new file)
  - `tests/unit/run_tests.sh`
- Functions/classes/components allowed to change: the new unit; the core source list in `tests/unit/run_tests.sh` only.
- Tests allowed or expected to change: the new unit test.

### Explicit Non-Goals
- No tracers, slaved sources or observers (Slice 6); no implicit scheme (Slice 7); no module, workspace or run-file knowledge; no compiler flag; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, xhigh effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_rate_integrator.c`, one case per criterion.
- Commands to run: the standing checks; `tests/unit/run_tests.sh test_rate_integrator` with its PASS markers read; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: record in `validation.md` each problem's measured orders, ratios and step counts beside its threshold.

### Rollback Path
- Revert the slice's commits; the unit and its test disappear and nothing else references them.

## Slice 6: Tracers, slaved sources and observers

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.2 (tracer moves and slaved sources), 3.6, 3.7 and 3.12, its decisions 6, 7, 10, 12 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement Design 3.6 in the Slice 5 unit: tracer moves at the parent transfer's coefficient times the tracer's own content, slaved sources evaluated after the transfers, observers accumulating the committed amount, the tracer-bound check and its failure cause `tracer_bound`. Terms the criteria use: u = 2⁻⁵³; L is the largest number of inflow plus outflow terms on one entry; N_acc the accepted steps; n_F the entries of total F; tol_F = 8u·[(L + 2)·N_acc + n_F + 2]·(Σ_F|x(start)| + Src_F + Snk_F); Θ_p is the parent's content at segment start plus the sum of the inflow amounts into it over the segment; the tracer bound is tracer − parent ≤ 8u·(L + 2)·N_acc·Θ_p at segment end; the error of a test problem is the largest absolute difference between a computed entry and its closed form at τ = 1.

### Acceptance Criteria
- [ ] Dilution: parent G₀ = 1 with a pristine source 0.5, tracer Z₀ = 0.02, Δ = 3: the concentration at τ = 1 equals Z₀/(G₀ + 0.5·Δ) to 1e-12 relative.
- [ ] Yield: G₀ = 1, Z₀ = 0.01, source 0.4 into G, formation G to S at k = 1.3, yield 0.02 slaved to formation into the tracer, Δ = 3, relative tolerance 1e-8: G and Z are within 10 × the relative tolerance of G = s/k + (G₀ − s/k)·e^(−kt) and Z = Z₀·e^(−kt) + y·(s/k)·(1 − e^(−kt)) + y·k·(G₀ − s/k)·t·e^(−kt).
- [ ] In the yield problem the tracer total closes within tol_F, with the yield counted as its external source.
- [ ] The yield amount equals 0.02 × the formed amount within 8u·N_acc relative.
- [ ] With an empty donor (G₀ = Z₀ = 0) every value is finite and the closed forms are met within 10 × the relative tolerance.
- [ ] A yield of 0.5 with no source fails with `tracer_bound`, and the message says the tracer has sources of its own.
- [ ] An observer equals the sum of the committed amounts of its flux bit for bit.
- [ ] An observer is excluded from the error norm, shown by identical step sequences with and without it.
- [ ] A transfer whose recipient has no tracer of the donor tracer's total records the tracer amount as a sink of that total, and the tracer total closes within tol_F.
- [ ] Every Slice 5 test passes with an unchanged outcome.

### Authorized Surface
- Files allowed to change:
  - `src/core/rate_integrator.c`
  - `src/core/rate_integrator.h`
  - `tests/unit/test_rate_integrator.c`
  - `tests/unit/test_rate_tracers.c` (new file)
- Functions/classes/components allowed to change: the integrator unit's flux kinds, tracer moves, observer accumulation and segment-end checks.
- Tests allowed or expected to change: the new tracer test; the Slice 5 test only where its component descriptions gain the new fields.

### Explicit Non-Goals
- No slaved chains; no implicit scheme; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_rate_tracers.c`.
- Commands to run: the standing checks; `tests/unit/run_tests.sh test_rate_tracers` and `tests/unit/run_tests.sh test_rate_integrator`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: record each measured error beside its threshold in `validation.md`.

### Rollback Path
- Revert the slice's commits; the unit returns to its Slice 5 state.

## Slice 7: The linearly implicit scheme

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.2 (the order of totals), 3.5 (the norm and controller), 3.7, 3.8 and 3.12, its Measured numerics, its decisions 1, 7, 10, 12 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement Design 3.8 behind the Slice 5 interface, selected by a scheme argument: Patankar–Euler with coefficients frozen at step start, the block lower-arrow solve in the subtraction-free (Grassmann–Taksar–Heyman) form, totals in the order of Design 3.2 (each after every total it depends on) with slaved amounts on the right-hand side, the filtered error estimate, controller exponent 1/2, and the fixed-step option. The first step is dt0 = h0, where h0 = 1 if d0 < 1e-5 or d1 < 1e-5 and otherwise min(1, 0.01·d0/d1), d0 and d1 as in Design 3.5 with zero-weight entries left out, and dt0 = 1 when every entry is left out; no probe state is formed and no extra law is evaluated, so no law ever sees a state the solve did not produce. Terms and systems the criteria use: u = 2⁻⁵³; the error of a test problem is the largest absolute difference between a computed entry and its closed form at τ = 1; N_acc and n_F as in Slice 5 (accepted steps, entries of total F); L is the larger of the largest number of inflow plus outflow terms on one entry and the number of entries in one galaxy's block; tol_F = 8u·[(L + 2)·N_acc + n_F + 2]·(Σ_F|x(start)| + Src_F + Snk_F); the tracer bound tracer − parent ≤ 8u·(L + 2)·N_acc·Θ_p with Θ_p the parent's start content plus its inflow amounts; the linear chain A to B to C at 1 and 0.3, Δ = 2, start (1, 0, 0); the bimolecular system A to C at 2·B and B to C at 2·A, start (1, 0.6, 0), Δ = 3; the yield problem G₀ = 1, Z₀ = 0.01, source 0.4 into G, formation G to S at 1.3, yield 0.02 slaved to formation into the tracer, Δ = 3.

### Acceptance Criteria
- [ ] On random star components (1 to 11 satellites, 2 to 6 entries per galaxy, coefficients spanning eight decades, a fifth of the right-hand sides zero) the solve matches a dense reference solve written in the test to 1e-10 relative for step × coefficient up to 1e3 (the reference's own rounding reaches 1e-12 there).
- [ ] On the same components, for step × coefficient up to 1e16, every solved entry is non-negative and each total closes within tol_F.
- [ ] With the number of entries per galaxy fixed, the solver's work and scratch grow linearly with the number of galaxies: per-galaxy blocks of R × R are the design, and no array or loop scales with the square of the number of galaxies, shown by a star of 10,000 satellites integrated inside a scratch bound the test states.
- [ ] Stiff pair (kf = kb, drain 1, Δ = 3, A₀ = B₀ = 0.5) at kf = kb = 1e3, 1e6, 1e9 and 1e12, relative tolerance 1e-3: accepted step counts are equal within one, and the relative error is at most 0.05.
- [ ] Bimolecular: the total closes within tol_F, and the A − B drift at 16, 64, 256 and 1024 fixed steps falls by a factor whose base-4 logarithm is at least 0.9 per quadrupling.
- [ ] Linear chain at fixed n = 16, 32, 64 and 128: the measured order is within 0.1 of 1 on each halving.
- [ ] Tracers stay within the tracer bound, and the tracer total closes within tol_F in the yield problem.
- [ ] The sink at c/x (x₀ = 0.5, c = 1, Δ = 1) that fails under the explicit scheme is integrated without failure under the implicit scheme, and the test records this as the reason the implicit scheme is never the default.
- [ ] A non-finite step × coefficient product fails with `bad_coefficient`.
- [ ] A state (1e-20, 1) whose small entry is drained at coefficient 1e12 is integrated with no law evaluated at a negative entry and no committed entry below zero.
- [ ] Every test system's values are literals in the test; random components come from a fixed-seed generator written in the test.
- [ ] Every explicit-scheme test passes with an unchanged outcome.

### Authorized Surface
- Files allowed to change:
  - `src/core/rate_integrator.c`
  - `src/core/rate_integrator.h`
  - `src/core/rate_implicit.c` (new file)
  - `tests/unit/run_tests.sh`
  - `tests/unit/test_rate_implicit.c` (new file)
  - `tests/unit/test_rate_integrator.c`
  - `tests/unit/test_rate_tracers.c`
- Functions/classes/components allowed to change: the integrator unit; the optional separate implicit file; its line in the core source list of `tests/unit/run_tests.sh`.
- Tests allowed or expected to change: the new implicit test; the Slice 5 and Slice 6 tests only to pass the scheme argument.

### Explicit Non-Goals
- No higher-order implicit scheme; no automatic switching between schemes; no dense matrix path in the unit; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, xhigh effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_rate_implicit.c`.
- Commands to run: the standing checks; `tests/unit/run_tests.sh test_rate_implicit`, `test_rate_integrator` and `test_rate_tracers`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: read the solve for any array or loop that scales with the square of the number of galaxies (per-galaxy R × R blocks are the design); record the stiff-pair step counts and the scratch bound in `validation.md`.

### Rollback Path
- Revert the slice's commits; the unit returns to its Slice 6 state with the explicit scheme only.

## Slice 8: The rate phase through real dispatch

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Frozen Facts on mode machinery, time, row order and the run file, its Design sections 3.2 (startup re-checks and the order of totals), 3.3, 3.4, 3.5 (Failure) and 3.9 (without the purity probe), its decisions 2, 3, 10, 11, 15, 17 and 19, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement Design 3.9 and the callback wiring of Design 3.3: parse and validate `modules.rate_integration` with its five keys `scheme`, `relative_tolerance`, `absolute_tolerance`, `max_steps_per_segment` and `max_rejections_per_step` (`src/core/read_parameter_file.c`, `struct MimicConfig`, defaults in `src/include/constants.h`, the key added to the `modules:` whitelist and to the reserved phase names); every startup refusal of Design 3.9, including a dependency cycle among the configured totals, plus the startup re-check of every configured transfer's end kinds against the reservoir table (Design 3.2); the integration domain from `to_record: central` and `reads_central` (Design 3.9) and the central view, non-NULL only for a module that declares `reads_central` (Design 3.3); assembly of a component from the workspace (gather, scratch rows, canonical order of decision 10, per-row Δ and the first-snapshot sentinel of Design 3.4); the rate pass inside `execute_phase()`'s event-dispatch window under the new running-callback kind; write-back on success; the refusal in `module_emit_event()`; the solver workspace in `MEM_UTILITY`.
- A failed segment ends the run through `FATAL_ERROR` with a one-screen message naming the cause, the module and transfer, the row and τ. No failure file is written yet (Slice 9).
- Add rate-phase and `modules.rate_integration` support to `tests/framework/harness.py`.
- Document the five run-file keys in the User Guide's Run File Structure section (its key reference) and in `.agents/skills/mimic-config-and-flags/SKILL.md` and its `references/all-config-keys.md`.

### Acceptance Criteria
- [ ] Each startup refusal of Design 3.9 (a phase mixing `process_rates` with another mode; `process_rates` in `pre_timestep`, `post_timestep` or `post_snapshot`; a second rate phase; a slaved source whose master's module is not configured in the rate phase; a dependency cycle among the totals of the configured rate modules, named in the message; a `modules.rate_integration` section with no rate phase, with an unknown key or with an out-of-range value; a configured transfer whose table names an end of the wrong kind, shown with a hand-built module) fails before any `init()` with a message naming the phase, module, key or totals, in a table-driven C unit test for the phase rules and an integration test for the run-file section.
- [ ] With the four rate fixtures in one phase on the default pair in a test build, the run completes under both `explicit` and `implicit`, some rows have non-zero values in each of the seven fixture properties, and no reservoir or tracer value in the output is negative or non-finite.
- [ ] One run with the four rate fixtures under `TimestepScheme: dynamic` on the default pair in a test build completes with non-zero fixture output and no negative or non-finite reservoir or tracer value.
- [ ] With no `to_record: central` transfer and no `reads_central` module configured, each galaxy is its own component: a galaxy's integrated state is bit-identical whatever the content of every other galaxy in its group, the central included (C unit test over a hand-built workspace).
- [ ] A hand-built probe module that declares `reads_central` receives a non-NULL central view holding the central's trial state and makes the group one component; one that does not declare it receives NULL (C unit test).
- [ ] With `test_rate_stripping` configured the whole group is one component.
- [ ] A group whose rows all carry the first-snapshot sentinel is unchanged by the rate phase and evaluates no law, and a non-sentinel row with `dT <= 0` is fatal naming the row (C unit test over a hand-built workspace).
- [ ] A rate callback that calls `module_emit_event()` with a context saved from an earlier discrete callback is refused with an error naming the module, and one that calls `module_create_record()` is refused (C unit test with hand-built probe modules).
- [ ] A hand-built probe law that returns NaN makes the segment fatal with a message naming the probe module, its transfer and cause `bad_coefficient` (a C unit test through `expect_fatal()` in `tests/framework/child_capture.h`, as existing unit tests of fatal errors do). That a failed segment leaves its input state untouched is Slice 5's criterion; here the assembly writes back only on success.
- [ ] A discrete phase after the rate phase that retires a row (hand-built probe) is followed by a rate segment that runs on the reduced row set.
- [ ] A record created in `pre_timestep` by `test_fixture` joins the component at zero content.
- [ ] The solver workspace is one tracked allocation, grown to its high-water mark and released in `module_system_cleanup()`, and the run's leak check is clean.
- [ ] A run file with no rate phase behaves exactly as before: `make tests-reference-identity REFERENCE_COMMIT=39a8d28e`, `make tests-snapshot-global`, `make tests-horizontal-v3` and `make tests-distributed` each exit 0.
- [ ] The rate phase runs on `MODEL=halos-only SIMULATION=mini-millennium-horizontal` (the horizontal driver) in a test build with non-zero fixture output.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `src/core/module_interface.h`
  - `src/core/read_parameter_file.c`
  - `src/include/types.h`
  - `src/include/constants.h`
  - `src/core/rate_assembly.c` (new file)
  - `src/core/rate_assembly.h` (new file)
  - `src/core/rate_integrator.h`
  - `tests/unit/run_tests.sh`
  - `tests/unit/tools/build_topology_dump.sh`
  - `tests/framework/harness.py`
  - `tests/unit/test_rate_dispatch.c` (new file)
  - `tests/integration/test_rate_phase.py` (new file)
  - `tests/unit/test_module_configuration.c`
  - `tests/unit/test_rate_mode_contract.c`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
- Functions/classes/components allowed to change: `execute_phase()`, the phase validators and visitors, `module_emit_event()`'s guards, `module_system_init()` and `module_system_cleanup()`, the phase plan; `parse_modules_section()`, `phase_name_is_reserved()` and a new section parser with its defaults; `struct MimicConfig`; the new assembly unit; the integrator header only for the interface the assembly needs; the core source lists of the two link scripts (the topology-dump list only if the parser's link needs it); the harness's `create_test_param_file()`; the User Guide's Run File Structure section.
- Tests allowed or expected to change: the named new tests; `test_module_configuration.c` and `test_rate_mode_contract.c` where phase rules change.

### Explicit Non-Goals
- No failure file, purity probe or `check_purity` key, and no fixture (Slice 9); no discrete-phase ledger, step statistics or provenance (Slice 10); no located events; no more than one rate phase; no edit to any shipped run file; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: core dispatch; public run-file keys; global dispatch state; module-system memory
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_rate_dispatch.c` (phase rules, domain, central view, sentinel, emit and create refusals, the NaN probe, row retirement, workspace); `tests/integration/test_rate_phase.py` (run-file section, both schemes, dynamic time-stepping, record creation, horizontal pair); the phase-rule cases in `test_module_configuration.c` and `test_rate_mode_contract.c`.
- Commands to run: the standing checks; `make TEST_BUILD=yes` then `tests/integration/test_rate_phase.py` on the default pair; `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration`, then the default pair restored; `make tests-snapshot-global`; `make tests-horizontal-v3`; `make tests-distributed`; `make tests-bench`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: read the NaN probe's fatal message; confirm the leak report of a rate-phase run is clean.

### Rollback Path
- Revert the slice's commits; the run-file section, dispatch and assembly disappear, and the integrator unit and metadata of Slices 2 to 7 stand unused.

## Slice 9: Failure diagnostics and the purity switch

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Design sections 3.5 (Failure), 3.9 (the purity probe) and 3.11, its decisions 10, 11, 15, 17 and 18, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Write the failure file of decision 15 on Slice 8's fatal path: `<output_directory>/rate_failure_task<N>.txt`, N the MPI task number (0 in a serial run), holding the component's segment-start state as hexadecimal floats, each row's Δ and the coefficients at the failing evaluation, written before `FATAL_ERROR` ends the run.
- Implement the purity probe of Design 3.9 and its `check_purity` key (decision 18: `true` or `false`, default `false`), parsed, validated and defaulted beside Slice 8's five keys in `src/core/read_parameter_file.c`, `struct MimicConfig` and `src/include/constants.h`, and documented with them in the User Guide's Run File Structure section, `.agents/skills/mimic-config-and-flags/SKILL.md` and its `references/all-config-keys.md`. When `check_purity` is `true`, at the first law evaluation of each segment the laws are evaluated a second time in reversed canonical order; every coefficient must match bit for bit and the scratch state must be byte-identical afterwards, or the run fails naming the module and transfer.
- Add the `test_rate_faults` fixture in its rate mode (Design 3.11: one sink from Cold whose law a parameter selects, well behaved, diverging as the donor empties, NaN, negative, or impure), with its README and `Makefile` source line, to `src/module_system/README.md`'s fixture list and to `tests/framework/harness.py` where the integration test needs it. Because it joins the global transfer index, `tests/unit/test_rate_transfer_tables.c` and `tests/integration/test_rate_metadata.py` change only where a test there must learn of it.

### Acceptance Criteria
- [ ] Under the explicit scheme, with `test_rate_faults` selecting the diverging coefficient, NaN and the negative coefficient in turn, each run exits non-zero, its message names the module `test_rate_faults`, its transfer and the cause, and the failure file exists in the output directory holding the component's segment-start state as hexadecimal floats, each row's Δ and the coefficients at the failing evaluation.
- [ ] With `test_rate_faults` selecting the impure law and `check_purity: true`, the run exits non-zero naming the module and the transfer.
- [ ] With the four well-behaved fixtures, `check_purity: true` gives `Galaxies` datasets byte-identical to the same run with it `false`.
- [ ] With `check_purity: false` the probe evaluates no law, shown by a C unit test with a hand-built probe law that counts its calls.
- [ ] `check_purity` with any value but `true` or `false` is refused at startup.
- [ ] Slice 4's and Slice 8's tests pass after the new fixture joins the global transfer index.
- [ ] A run file with no rate phase behaves exactly as before: `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` exits 0.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_registry.c`
  - `src/core/rate_assembly.c`
  - `src/core/rate_assembly.h`
  - `src/core/read_parameter_file.c`
  - `src/include/types.h`
  - `src/include/constants.h`
  - `Makefile`
  - `src/module_system/test_rate_faults/` (new directory)
  - `src/module_system/README.md`
  - `tests/framework/harness.py`
  - `tests/integration/test_rate_phase.py`
  - `tests/unit/test_rate_dispatch.c`
  - `tests/unit/test_rate_transfer_tables.c`
  - `tests/integration/test_rate_metadata.py`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
- Functions/classes/components allowed to change: the rate pass's failure path and the purity probe in `execute_phase()` and the assembly unit; the `modules.rate_integration` parser, its defaults and `struct MimicConfig` for `check_purity` only; one `Makefile` fixture source line; the new fixture; the fixture list in `src/module_system/README.md`; the harness's rate-integration support; the User Guide's Run File Structure section; the configuration skill's key rows.
- Tests allowed or expected to change: the named tests.

### Explicit Non-Goals
- No replay tool; no full-halo mode of `test_rate_faults`, discrete ledger, statistics or provenance (Slice 10); no change to the integrator's numerics; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: core dispatch; public run-file keys
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/integration/test_rate_phase.py` (the three faults, the failure file, the purity switch and its refusal); the probe-count case in `tests/unit/test_rate_dispatch.c`; `tests/unit/test_rate_transfer_tables.c` and `tests/integration/test_rate_metadata.py` only if the new fixture requires it.
- Commands to run: the standing checks; `make TEST_BUILD=yes` then `tests/integration/test_rate_phase.py` on the default pair in a test build, then the default pair restored; `tests/unit/run_tests.sh test_rate_dispatch` and `test_rate_transfer_tables`; `make tests-bench`; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: read one failure file and the failing run's message; record in `validation.md` whether Slice 4's or Slice 8's tests changed and why.

### Rollback Path
- Revert the slice's commits; the failure file, the purity probe, `check_purity` and the faults fixture disappear, and Slice 8's dispatch stands, ending a failed segment with its message alone.

## Slice 10: The discrete-phase ledger, step statistics and provenance

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Frozen Facts on provenance, its Design sections 3.2 (`changes_totals`), 3.7, 3.10 and 3.11, its decisions 9, 10, 11, 16, 17 and 18, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Implement the discrete half of Design 3.7 with the `changes_totals` key (generator, validator and generated binding): only when a rate phase is configured, the core sums each conserved total over the FoF workspace's active rows before and after each dispatch unit of a discrete phase, a dispatch unit being one full-halo module's call with the per-event consumers it triggers, or the phase's by-galaxy pass as a whole. A change beyond 8u·(n_F + 2)·Σ_F|x| in a full-halo unit is fatal unless that full-halo module, or a per-event module configured in the phase that subscribes to an event that module emits, declares the total; a change in the by-galaxy pass is fatal unless a by-galaxy module configured in the phase declares it; a declared change is counted in the step statistics.
- Implement Design 3.10: `RunProperties/RateTransfers` in master and partition files, `RunProperties/RateIntegration` in the master file, the step statistics merged at run end and summarised in one `INFO_LOG` line, all written only when a rate phase is configured.
- Add the `test_rate_jump` fixture and the full-halo mode of `test_rate_faults` (Design 3.11).
- Document the output records in the User Guide's Reading HDF5 Output section and in `.agents/skills/mimic-run-and-operate/SKILL.md`, and the `changes_totals` key in the Developer Guide's Module Metadata Schema and in `.agents/skills/mimic-modules/SKILL.md`.

### Acceptance Criteria
- [ ] With `test_rate_jump` merging without loss, the run completes and the ledger records no change.
- [ ] With a declared loss, the run completes and the per-total declared-change count is non-zero.
- [ ] With `test_rate_faults` adding mass in a discrete phase without declaring it, the run exits non-zero naming the phase and the total.
- [ ] With `test_rate_jump` (declaring both totals) and `test_rate_faults` in its full-halo mode (adding mass undeclared) configured in the same discrete phase, the run exits non-zero naming `test_rate_faults` and the total.
- [ ] A declaration by a per-event consumer covers only the full-halo modules whose events it subscribes to: with hand-built modules in one discrete phase, a consumer that declares the total and subscribes to producer A does not excuse an undeclared change by an unrelated full-halo module B, which is fatal naming B (C unit test).
- [ ] With no rate phase configured no discrete-unit sum is computed, shown by the byte gate and by a unit test that the hook is not entered.
- [ ] `RunProperties/RateTransfers` and `RunProperties/RateIntegration` exist exactly when a rate phase is configured (`RateTransfers` in the master and every partition file, `RateIntegration` in the master only), with the scheme, the four numeric key values, `check_purity` and the method string, and one `RateTransfers` row per configured transfer (integration test reading the HDF5).
- [ ] The step statistics satisfy their own arithmetic: each histogram's counts sum to the segment total, and in a C unit test with a hand-built probe law that counts its own calls, the recorded segments equal the (component, substep) pairs the test ran and, with every component built to hold the same number of active rows, the recorded law evaluations times that number equal the probe's call count.
- [ ] `make tests-bench` passes, with the `Parameters` dataset unchanged.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e`, `make tests-snapshot-global-identity` and `make tests-distributed` each exit 0.
- [ ] Under `mpirun -np 2` on a test build of `MODEL=halos-only SIMULATION=mini-millennium-horizontal` with the `forest_blocks` fixture, the master file's statistics equal the serial run's exactly.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `src/core/module_interface.h`
  - `src/core/rate_assembly.c`
  - `src/core/rate_assembly.h`
  - `src/io/output/metadata_hdf5.c`
  - `src/io/output/master_hdf5.c`
  - `src/io/output/hdf5_internal.h`
  - `src/core/main.c`
  - `scripts/generate_module_registry.py`
  - `scripts/validate_modules.py`
  - `scripts/module_modes.py`
  - `Makefile`
  - `src/module_system/test_rate_jump/` (new directory)
  - `src/module_system/README.md`
  - `src/module_system/test_rate_faults/`
  - `tests/integration/test_rate_phase.py`
  - `tests/integration/test_rate_provenance.py` (new file)
  - `tests/integration/test_rate_metadata.py`
  - `tests/unit/test_rate_dispatch.c`
  - `tests/integration/test_output_formats.py`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
- Functions/classes/components allowed to change: the discrete-phase hook around `execute_phase()`; the assembly unit's statistics; two new static writers called from `store_run_properties()` and `write_perfile_metadata()`; the run-end reduction in `main.c` only; `changes_totals` handling in the two tools and their shared helpers (`scripts/module_modes.py` only if the shared rate-metadata rules live there); one `Makefile` fixture source line; the faults fixture's full-halo mode; the fixture list in `src/module_system/README.md`; the User Guide's Reading HDF5 Output section; the Module Metadata Schema section.
- Tests allowed or expected to change: the named tests.

### Explicit Non-Goals
- No unconditional attribute; no change to `hdf5_format_version`; no statistics in binary output; no floating-point sum in output; no attribution of a discrete change finer than the dispatch unit; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: HDF5 output metadata; MPI reduction at run end
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/integration/test_rate_provenance.py` (presence and absence, contents, arithmetic); jump, undeclared-change and mixed-unit cases in `tests/integration/test_rate_phase.py`; `changes_totals` refusals in `tests/integration/test_rate_metadata.py`; the hook case in `tests/unit/test_rate_dispatch.c`; absence assertions in `tests/integration/test_output_formats.py`.
- Commands to run: the standing checks; `make tests-bench`; `make tests-snapshot-global-identity`; `make tests-distributed`; `make MODEL=halos-only SIMULATION=mini-millennium-horizontal TEST_BUILD=yes USE-MPI=yes`, then a serial and an `mpirun -np 2` run of a scratch run file derived from `forest_blocks_halos-only.yaml` with the rate phase added, written outside the repository, with the master files' statistics compared, then the default pair restored; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: `h5ls -r` of a rate-phase master file and of a run without one.

### Rollback Path
- Revert the slice's commits; the discrete ledger, statistics, provenance, jump fixture and the faults fixture's full-halo mode disappear, and Slices 8 and 9 stand.

## Slice 11: The rate battery

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Frozen Facts on row order and on gates and suites, its Repository Evidence on batteries, its Design sections 3.9, 3.10 and 3.11, its decisions 10, 14, 16 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Add `tests/manual/run_rate_battery.py` and `make tests-rate`, modelled on `tests/manual/run_snapshot_global_battery.py` and `tests/manual/test_distributed_identity.py`: it builds its own test-build pairs, runs its legs, fails on any SKIP or NA, and restores the caller's generated code.
- Add a CI step that runs the legs that need no external data, in the `horizontal-v3` job of `.github/workflows/ci.yml` (`:97`), the job that already runs `make tests-distributed` with `MPIRUN: "mpirun --oversubscribe"` set on that step (`:160-164`); the new step sets the same `MPIRUN`.
- Document the target in `tests/README.md`, the Developer Guide's Testing section and `.agents/skills/mimic-validation-and-qa/SKILL.md`.

### Acceptance Criteria
- [ ] Cross-driver leg: the four rate fixtures plus `test_rate_jump` on `halos-only` over the real mini-Millennium data, vertical and horizontal, under the explicit scheme with fixed and with dynamic time-stepping and under the implicit scheme with fixed time-stepping (three pairs of runs), give per-`UniqueGalaxyID` byte identity of every output field through `scripts/compare_cross_format_identity.py`, and identical step statistics.
- [ ] Row-order leg: for each FoF group in each of those pairs of runs, the sequence of tree rows in the output is the same under both drivers.
- [ ] Task and chunk leg: on the `forest_blocks` fixture, a non-MPI serial run against `mpirun -np 1, 2, 3, 4, 8` and against `input.forest_chunks` 2, 3 and 8, with `--compare-created`, the per-partition row-order check and identical step statistics.
- [ ] Permutation leg, on the `forest_blocks` fixture: the four rate modules listed in at least four different orders in the run file, including the reverse, give byte-identical `Galaxies` datasets and row order on one binary under each scheme; the comparison excludes `EnabledModules` and the run-file copy, which record the configured order.
- [ ] Record-creation leg, on the `forest_blocks` fixture under one driver: `test_fixture` creating records in `pre_timestep` ahead of the rate phase runs clean and is identical across task counts with `--compare-created`.
- [ ] Every leg first asserts that the fixture properties are non-zero in some rows.
- [ ] `make tests-rate` exits 0 locally.
- [ ] The CI step, in the `horizontal-v3` job with `MPIRUN: "mpirun --oversubscribe"` set as the distributed gate's step sets it, runs the task-count, chunk and permutation legs.
- [ ] `make tests-snapshot-global`, `make tests-distributed` and `make tests-horizontal-v3` each still exit 0.

### Authorized Surface
- Files allowed to change:
  - `tests/manual/run_rate_battery.py` (new file)
  - `Makefile`
  - `.github/workflows/ci.yml`
  - `simulations/mini-millennium-horizontal/_tests/input/` (new run files only)
  - `tests/README.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `tests/framework/harness.py`
- Functions/classes/components allowed to change: the new battery; the `Makefile` target, `.PHONY` entry and help text; one CI step; new run files beside the committed `forest_blocks` ones; the manual-tests paragraph and target list of `tests/README.md`; the Developer Guide's Testing section; the validation skill's gate table; a harness helper only if the battery shares it.
- Tests allowed or expected to change: the new battery.

### Explicit Non-Goals
- No change to `tests/framework/parity_gate.py` or to the comparator; no cross-driver comparison of created records; no fuzzer change; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: the Makefile; the CI workflow
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the battery and its legs.
- Commands to run: the standing checks; `make tests-rate`; `make tests-snapshot-global`; `make tests-distributed`; `make tests-horizontal-v3`, then the default pair restored; `make tests-converter`; `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice.
- Lint (differential, via the lint skill): required.
- Manual checks: read the battery log for each leg's case count and the non-zero assertion; read the CI step against the legs it names.

### Rollback Path
- Revert the slice's commits; the battery, target, CI step and run files disappear.

## Slice 12: The gate and the cost of the mode

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Outcome and Limits (the brief's gate and where each item is settled), its Repository Evidence on cost, its decisions 4, 9, 14 and 17, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Run the brief's whole gate on the committed branch, measure the cost of the mode on the fixture, and write `docs/dev/MIMIC-COUPLED-RATE-ACCEPTANCE.md`.
- Add a small manual cost driver, `tests/manual/rate_cost.c`, that times hand-built stars through the integrator alone. The battery's `--cost` option compiles and runs it with an explicit source list, as `tests/manual/test_distributed_identity.py` compiles its MPI control test (`CONTROL_SOURCES`, `:136-138`; the compile command, `:289-292`), invoked as `python3 tests/manual/run_rate_battery.py --cost`. No `Makefile` target is added.

### Acceptance Criteria
- [ ] `make tests` passes for `sage16`, `sage26`, `halos-only` and `hod` on `mini-millennium`, and for `sham` on `micro-uchuu-ascii-horizontal`, each run alone with `MIMIC_BASELINE_RTOL` unset, so the baselines are strict, with exit codes recorded.
- [ ] `make generate && make check-generated` passes in production mode for every distinct `(model.name, simulation.name)` pair the run files under `models/*/input/` declare (51 files, 50 distinct pairs at the baseline), and the default pair is restored afterwards.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=39a8d28e`, `make tests-distributed`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `make tests-horizontal-v3`, `make tests-rate`, `make tests-converter` and `make tests-bench` each exit 0.
- [ ] Cost, integrator alone: stars of 10, 100, 1,000 and 10,000 satellites, explicit and implicit, at least five timed repeats after one discarded: time per galaxy per law evaluation at 10,000 is at most 1.5 times that at 100, with the accepted steps per segment reported for each size.
- [ ] In the same runs, solver workspace bytes per galaxy at 10,000 are within 10% of those at 1,000.
- [ ] Cost, real dispatch: wall time and peak RSS of the rate fixture pipeline on mini-Millennium under both drivers against the empty `halos-only` pipeline, five repeats each through `scripts/profiling/run_wall.py` from the repository root, are reported with their spread; no threshold applies and no production figure is claimed.
- [ ] The acceptance record states, for each of the brief's seven gate items, the evidence and where it lives, every command with its exit code, the cost tables, the machine and compiler, and anything that did not pass.
- [ ] A gate that fails is reported as failed with its output; nothing is loosened to pass.

### Authorized Surface
- Files allowed to change:
  - `docs/dev/MIMIC-COUPLED-RATE-ACCEPTANCE.md` (new file)
  - `tests/manual/rate_cost.c` (new file)
  - `tests/manual/run_rate_battery.py`
- Functions/classes/components allowed to change: the acceptance record; the cost driver; a `--cost` option in the battery that compiles and runs it.
- Tests allowed or expected to change: none beyond the cost driver and the `--cost` option.

### Explicit Non-Goals
- No change to runtime source or the `Makefile`; no tuning of the integrator to meet a cost figure; and nothing the standing rules forbid.
- Documents of record that Slice 13's table lists are not updated here, beyond the reference entries this slice's Intended Change names (decision 11); a reviewer should not raise their staleness against this slice.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the cost driver and the battery's `--cost` option.
- Commands to run: every command in the criteria, one at a time, each delegated where it runs over a minute and its exit code recorded, every `make tests` run with `MIMIC_BASELINE_RTOL` unset; the standing checks; `python3 tests/manual/run_rate_battery.py --cost`.
- Lint (differential, via the lint skill): required.
- Manual checks: nothing else runs on the machine during a timing; the record names the commit measured, which changes no runtime source.

### Rollback Path
- Revert the slice's commits; the record, cost driver and `--cost` option disappear, and the runtime is untouched.

## Slice 13: The documents of record

### Intended Change
- Binding context: read `docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`. Its Outcome and Limits, its Design sections 3.1 to 3.11, its decisions 2, 6, 7, 9, 11, 15, 18 and 19, and the standing checks and standing rules under Execution Structure bind this slice and its review.
- Edit `docs/VISION.md` with the frozen text below, and nothing else in it.
- Describe the mode in the Developer Guide's processing-mode, phase and callback sections (Module Lifecycle, a rate callback contract beside the Snapshot Callback and Record Creation contracts, Processing Modes, Phase Order, Code Generation), in the User Guide's module-configuration narrative (How Mimic Processes a Run, The Physics Pipeline, Troubleshooting), in `AGENTS.md`'s module-system boundary, in `docs/STYLE-GUIDE.md`'s naming and metadata lines, and in `src/module_system/README.md` and the template's README and commented `template_module_info.yaml`.
- Sweep the skills that own the architecture contract, modules, properties, configuration keys, run operation, debugging, validation, change control, writing and diagnostics. Add one `CHANGELOG.md` entry under Unreleased; update the pathway and the brief's status line.
- Correct every statement the rate mode makes false. At the baseline these are:

| File | Where | Statement made false or incomplete |
|---|---|---|
| `docs/VISION.md` | Principle 4 | The three FoF dispatch modes; "rate or batch callbacks remain prospective" |
| `docs/DEVELOPER-GUIDE.md` | Processing Modes | "The first three modes form the FoF callback family"; FoF phases accept only FoF modes |
| `docs/DEVELOPER-GUIDE.md` | Phase Order | "Inside each phase" lists three steps; the phase plan splits entries into three lists |
| `docs/DEVELOPER-GUIDE.md` | Debugging, broken module startup | "Use `process_full_halo`, `process_by_galaxy`, or `process_per_event`" (already omits `process_snapshot`) |
| `docs/DEVELOPER-GUIDE.md` | Module Metadata Schema | `supported_processing_modes` drawn from a list of four |
| `docs/USER-GUIDE.md` | How Mimic Processes a Run | Modules grouped into three processing modes; "Full-halo modules always run before by-galaxy modules within a phase" |
| `AGENTS.md` | Load-bearing boundaries, module system | Lists three FoF enumerators and two callback families |
| `docs/STYLE-GUIDE.md` | Naming (C, Metadata) | Callback names and mode strings for the FoF and snapshot families only |
| `.agents/skills/mimic-architecture-contract/SKILL.md` | Principle 4; data flow; `struct PhasePlan` row | "three FoF dispatch modes"; the three-list phase plan and its dispatch order |
| `.agents/skills/mimic-modules/SKILL.md` | Processing modes; schema table | "The first three modes are the FoF family"; the ordering law; a subset of four mode strings |
| `.agents/skills/mimic-config-and-flags/SKILL.md` | `modules.phases` row | Three modes listed |
| `.agents/skills/mimic-config-and-flags/references/all-config-keys.md` | `modules:` section | "listing the four" modes; the accepted `modules` keys |
| `.agents/skills/mimic-run-and-operate/SKILL.md` | Run-file anatomy | Dispatch grouped by mode: full-halo, events, by-galaxy |

Frozen `docs/VISION.md` text:

- Principle 2, the requirement "Scientific ordering remains explicit in configuration. Metadata validation catches wiring errors but does not replace scientific judgement." becomes: "Scientific ordering remains explicit in configuration wherever order has meaning: phases run in the order listed, and a discrete phase's modules run in their configured order within its dispatch rules (full-halo modules, with the event consumers they trigger, before by-galaxy modules). The modules of a rate phase are the one exception, by design: they declare transfers that the core integrates as one system, so their order in the run file does not affect the result. Metadata validation catches wiring errors but does not replace scientific judgement."
- Principle 3, the generated-code requirement becomes: "Generated code provides C struct fields, output schema writers, HDF5 field metadata, module registration, event identifiers, the reservoir table, and each rate module's declared transfers." A new requirement follows it: "Reservoirs, tracers and observers are declared in property metadata, and a rate module declares both ends of each transfer in `module_info.yaml`. A transfer that names an undeclared reservoir fails before a run starts."
- Principle 4, the dispatch-mode requirement becomes: "`process_full_halo`, `process_by_galaxy`, `process_per_event` and `process_rates` are dispatch modes within this model, not separate tree-processing algorithms." A new requirement follows it: "A `process_rates` module declares conservative transfers between reservoirs and returns their rate coefficients from a callback that receives read-only state. It never updates state, emits an event or creates a record. A phase is either a rate phase, holding only rate modules, or a discrete phase, and at most one rate phase is configured. The core integrates the rate phase's assembled system across each substep with error control, applies each transfer's amount to both of its ends, checks that every conserved total closes, and fails rather than commit a negative reservoir. The result does not depend on the order of the rate modules in the run file, on the driver, or on the MPI task or chunk count." The snapshot requirement's last sentence becomes: "Further snapshot-wide modes, such as snapshot-wide rate or batch callbacks, remain prospective and would each need their own explicit contract." The "In practice" paragraph gains, after its event-consumer sentence: "A rate module receives a read-only copy of one galaxy at a trial state and returns the rate coefficients of the transfers it declares; the core integrates the transfers of every rate module in the phase together."
- Principle 6, the HDF5 requirement becomes: "HDF5 output records field metadata, enabled modules, model parameters, redshift mapping, version information, and event contracts when present, and, when a rate phase is configured, the integrator, its tolerances and budgets, the declared transfers and the run's step statistics."
- Data flow step 2, "Property and module metadata for the selected model and simulation generate C structs, output metadata writers, module registration, and event identifiers.", becomes: "Property and module metadata for the selected model and simulation generate C structs, output metadata writers, module registration, event identifiers, the reservoir table, and each rate module's transfer tables."
- Data flow step 4, "The configured phases are checked against registered modules, supported modes, and event contracts.", becomes: "The configured phases are checked against registered modules, supported modes, event contracts, and, for a rate phase, the declared transfers."
- Data flow step 6 gains, after its first sentence: "A rate phase is integrated as one system across each substep."

### Acceptance Criteria
- [ ] `docs/VISION.md` carries the frozen text exactly, and no principle other than 2, 3, 4 and 6 and no data-flow step other than 2, 4 and 6 changes.
- [ ] Every statement in the table above is corrected.
- [ ] A search of the repository outside `docs/dev/` and `archive/` for "three FoF dispatch modes", "three dispatch modes" and "first three modes" finds none that is still wrong.
- [ ] The Developer Guide documents the rate callback contract, the rate-law contract (what a coefficient means for each declaration kind, purity, no side effects), `reads_central`, the phase rules, the integration domain, mixed elapsed times, the ledger, the per-unit attribution of discrete changes, the `check_purity` switch and the failure causes, in the sections that document the existing modes.
- [ ] The Developer Guide states that a `post_snapshot` module that writes a reservoir does so outside the ledger.
- [ ] `CHANGELOG.md` has one Unreleased entry that says what the mode is and what is not claimed: proven on a fixture, no model package uses it, no production cost figure.
- [ ] The pathway shows stage 1's code complete with the release still owed, lists the acceptance record and adds a changelog row; the brief's status line says its stage 1 is implemented.
- [ ] Nothing outside `docs/dev/` references a `docs/dev/` document.
- [ ] `make check-docs` and `make check-format` pass.

### Authorized Surface
- Files allowed to change:
  - `docs/VISION.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `docs/STYLE-GUIDE.md`
  - `AGENTS.md`
  - `README.md`
  - `CHANGELOG.md`
  - `tests/README.md`
  - `src/module_system/README.md`
  - `src/module_system/template/README.md`
  - `src/module_system/template/template_module_info.yaml`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-properties/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-change-control/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`
- Functions/classes/components allowed to change: prose and tables only; in `template_module_info.yaml`, comments only; `README.md` only if it lists the modes.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No restatement of the generated tables; no archiving of this plan (the owner's step after the run); no code change; and nothing the standing rules forbid.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: the standing checks; the phrase search of the criteria; `grep -rn "docs/dev/" --exclude-dir=docs --exclude-dir=archive .` read for document references; `make tests-converter` and `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` on the committed slice, because it edits files under `src/`.
- Lint (differential, via the lint skill): required.
- Manual checks: read `docs/VISION.md`'s diff against the frozen text word for word; read each skill's changed section against the code on the branch.

### Rollback Path
- Revert the slice's commits; the documents of record return to the branch's Slice 12 state.

---

## Next Chat Prompts

### Mode A: Human-managed plan

Recommended only as the fallback; Mode B is the intended way to run this plan.

```md
Plan file: /Users/dcroton/Local/git-repos/mimic/docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md
Scope: all remaining slices, in plan order
Supervision: checkpointed — I am present: ask me before coding an approval-flagged slice and before each commit
Reviewers (optional): drift auditor claude claude-sonnet-5-5 high; code reviewers claude claude-opus-5-5 high; codex gpt-6.1-sol high

Read the full plan file first. If a slice or batch receipt in scope is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer (the reviewers above, when given) only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass. If no Reviewer is configured or available, perform Developer self-audit on a default slice and record that provenance explicitly; for a slice whose Risk Flags say `Independent audit required: yes`, stop and report instead of self-auditing.

For each slice or batch in scope, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If any included slice's Risk Flags mark approval-needed, stop before coding: checkpointed, get my approval; autonomous, end the run and report — never self-approve a slice the plan gated for a human.
3. apply the scoped-implementation skill against the selected contract.
4. apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit. Report the authorization gate result and who performed it before any quality review.
5. If the gate fails, fix the drift inside the contract and re-audit; if it cannot be fixed inside the contract, stop and report.
6. On a passing gate: for a broad or structural change, first run the code-health skill differentially against the slice's starting commit and supply its report as review evidence. Then apply the code-review skill using the code reviewers when available, in parallel if there are several; otherwise perform Developer self-audit through the code-review skill. Record who performed it.
7. Collect every finding from the current audit or review round before fixing (checkpointed, surface them to me first), record them in the slice summary, fix them as one batch, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged, record residuals in the slice summary and proceed.
8. Commit with the commit skill: checkpointed, ask me first; autonomous, commit a slice that has passed validation, the drift-audit gate and the code-review gate — and only such a slice.

Stop early on: a blocker, an unapproved scope change, a gate or validation failure unfixable inside the contract, or context pressure. When the scope is done, or on any stop with slices remaining, use the handoff skill to record state, audit provenance (Reviewer tool/label or Developer self-audit and fallback context), and the next slice or batch to resume from. Do not continue past the scope.

When the scope was all remaining slices and the plan is complete, use the report skill instead to write a final report covering slices committed, gate results and audit provenance per slice, validation, and every residual or post-plan consideration left for me to assess. Do not lose a non-blocking observation merely because it did not belong in the frozen plan.

Confirm before starting: plan file read, scope, supervision, branch, and the first slice. Then begin.
```

### Mode B: PM-managed plan

```md
Plan file: /Users/dcroton/Local/git-repos/mimic/docs/dev/MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: claude per plan (each slice's Recommended Developer line)
Drift auditor: claude claude-sonnet-5-5 high
Code reviewers: claude claude-opus-5-5 high; codex gpt-6.1-sol high
Run tag (optional): coupled-rate-mode
Use the project-manager skill. You are the PM: the accountable supervisor of this run. You never write slice code.

Handoff: HANDOFF.md in the repo root, git-ignored (add it to .git/info/exclude if it is not; a campaign child's is already excluded), is the sole entry point for a restart and for my post-run review. Create it after init and update it after every slice decision and before any stop with: run identity (never the token), seats, branch, commit authorisation, review protocol, slice log, plan decisions, residuals and follow-ups, and PM lessons.

Starting the run: `init` takes full model ids, efforts where the harness supports them, the run tag and your own model and effort as `--pm-model`; set `PM_TMUX_SOCKET` exactly as the `export PM_TMUX_SOCKET=…` line init prints, for every later command of the run.

Human present (the default):
- If HANDOFF.md names an unfinished run of this plan, read it fully and resume that run (I will supply the token). Otherwise prepare, then confirm with me once:
  - Read the whole plan, run check-plan with repo context, and check that the repo still matches what the plan assumes. Init freezes the plan, so raise any drift or warning now. Run check-plan and init without `--policy`. Confirm that the real mini-Millennium data under /Volumes/Internal/data/millennium/ resolves through both packages' `snapshots` links, which the byte gate needs. Every suite runs under the default `TMPDIR`.
  - Require a clean worktree with the plan committed. Propose the plan's feature branch, or a new branch name to create.
  - Confirm: plan read, check-plan result, seats, branch, first slice, any approval-flagged slices (each stops the run until I approve it), my standing authorisation for Developers to commit each slice, and, when the Developer's harness asks for directory trust per worktree, my permission to add the repo's trust entry yourself (project-manager's references/sandbox.md *Troubleshooting*).
- After my go-ahead, run init. Give me the run id and token, then begin.

Per slice, in plan order:
1. start-slice with the Developer seat's model and effort. Wait with one long background `observe --wait`; never poll.
2. Run finalize, then your own checks: pane, diff, validation output, full test suite, lint, differential code-health if the structure changed, and a style-guide audit (the project's guide, otherwise the baseline). Treat newly introduced, in-contract style deviations as cleanup to steer.
3. Review as project-manager's *Risk* section sets: on a standard slice your own checks in step 2 are the review, and commissioning more is your recorded review-depth discretion; an elevated slice (`Independent audit required: yes` or a risky surface) commissions the drift audit first, which you read and rate, then the code reviewers, who share one `--adjudicated-file`. Let reviews run to completion without watching them.
4. Rate every commissioned report (with its confirmed-defect count), order the code panel when there is one, and score the Developer (criteria met, confirmed defects).
5. Decide: accept, steer or stop. Steer only once every review is in, with all findings batched into one correction. Each follow-up round re-runs what the change needs: on an elevated slice, both reviews fresh at the new head, focused on the change since the last round through an adjudication line.
6. Update `pm notes` for the next Developer, and HANDOFF.md for me.

Plan defects: where the plan's goals make one resolution natural and its impact stays within the slice or touches only a few slices, decide it yourself and record it as a delegated plan decision. Never edit the plan, widen the surface except by grant, or relax a floor fact. Stop for anything serious, contested or cross-cutting, and whenever the floor or a plan gate needs a human.

Finish: run `status --report`, confirm the ledger was written, run `ledger render`, and read `run-report.md` end to end. Record the run's actual status in HANDOFF.md. Report:
- total run time, quoted from the report with its endpoints
- commits (unpushed)
- what was accepted, and on what evidence
- what stopped, and why
- plan defects, with the decisions you took and the amendments you recommend
- residual risks and follow-ups, with a convergence read
- the leaderboard path
```
