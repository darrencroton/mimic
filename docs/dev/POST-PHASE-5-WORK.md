# Post-Phase-5 Work

**Status:** Open. Written 2026-08-12 at branch `feature/ctrees-snapshot-reader`, HEAD `168aa7fd`, immediately after dual-driver Phase 5 closed. **§6 reconciled 2026-08-29** against the closures the subset rehearsal and the converter scale pass produced — items 2, 3, 6 and 9 closed from measurement, with the obligations those closures created carried explicitly under "Carried into the production run". **Item 10 is the only §6 item still open.**
**Purpose:** The single place every item deferred, reported-but-not-fixed, or newly discovered during Phase 5 is recorded with enough context to act on without re-reading the plan or its run records. Nothing here blocks Phase 5's closure; several items block or shape the Shin-Uchuu production conversion, and those are marked.
**Provenance:** Phase 5 ran under `project-manager` (Mode B) across four PM runs — `20260811T055422Z-02e67d` (Slices 1–9), `20260812T072526Z-bbdfaf` (Slice 10), `20260812T130721Z-bd8cff` (Slice 11), plus an earlier run whose slices were attested forward. Their assessments, validation records and review reports live under `.pm/runs/<run-id>/` and are the evidence behind every claim below.

---

## 1. What Phase 5 actually delivered

Recorded here because several items below only make sense against it, and because the Shin-Uchuu executor needs it in one place.

- **A second live driver.** `run_snapshot_driver()` (`src/core/snapshot_driver.c`), dispatched from `run_processing_driver()` (`src/core/tree_driver.c`) when `input.processing_order: snapshot_ordered`. That case was previously a `FATAL_ERROR`. Weak points **W1 and W2 are closed**.
- **A cross-format identity gate that passes bitwise.** Tree-ordered versus snapshot-ordered over the full micro-Uchuu dataset: identical `UniqueGalaxyID` sets and per-ID byte-identical fields for every output snapshot, for `halos-only` (4,409,282 records, 20 fields) and `sage16` (3,111,793 fixed / 3,111,759 dynamic, 42 fields), under both timestep schemes, aggregating the tree side's five output partitions against the snapshot side's one. **No tolerance of any kind** — the comparison is raw bytes. (**Note added 2026-08-13 (D5(a)):** "the snapshot side's one" records what this gate run aggregated at the time. Since D5(a) the snapshot side writes one partition per requested output snapshot, and the same gate now reports `tree-ordered 5 partition(s) vs snapshot-ordered 8 partition(s)`; the bitwise identity result is unchanged.)
- **The tree-ordered path is unchanged.** Galaxy records byte-identical to the pre-Phase-5 baseline `ae22d278`, with exactly four permitted HDF5 metadata deltas (`UniqueGalaxyIDMultiplier` attribute; `TotHalosPerSnap` widened to int64; the `UniqueGalaxyID` description; `hdf5_format_version` 1.1 → 1.2) and `output_schema.json` differing in exactly the description and `source_md5`.
- **Supporting seams:** an explicit `struct HaloInputView` replacing the `InputTreeHalos` global; identity fields on reader-owned slab arrays; physical-header agreement at `open_run`; an instanced `struct GalaxyPool *`; int64 output widths; a configured `UniqueGalaxyID` multiplier honoured by **both** drivers and recorded in output provenance; a driver-neutral output-partition seam.
- **Snapshot-run constraints, enforced at config time** (`src/core/read_parameter_file.c:1475-1488`): HDF5 output only, serial only (`NTask > 1` rejected), `--skip` rejected. Snapshot runs carry no `Ntrees` and no `TreeHalosPerSnap`, and use int64 `TotHalosPerSnap`.

  **Updated 2026-08-13 (D5(a)).** This bullet previously said snapshot runs write **one** output partition plus a master. They now write **one partition file per requested output snapshot**, named by that snapshot's number (`model_<snapnum>.hdf5`, `%03d`-padded), plus the master — and cleanup is per-partition rather than all-or-nothing, so a partition file that has closed successfully survives a later failure. Operationally that is the difference between losing a whole multi-week run's output to a late failure and losing only the snapshot in flight. See `SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` and `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver".

**How to re-run the gate** — a manual, dataset-present operation; no automated tier and no CI runs it:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific
```

Roughly six minutes and ~7 GiB of scratch on the development machine. It builds four isolated git worktrees, runs eight comparisons plus a baseline preservation stage, and cleans up on every exit path.

---

## 2. Blocks or shapes the Shin-Uchuu conversion

Work through this section before converting Shin-Uchuu.

### 2.1 `simulations/uchuu/halo_properties.yaml` declares `Spin` with a bound its own data will exceed — **highest priority**

`Spin` in the Consistent-Trees packages is **J/Mvir** — specific angular momentum in (Mpc/h)(km/s) — despite a `dimensionless` units label. It scales with halo size. `simulations/uchuu/halo_properties.yaml` bounds it at `[-20.0, 20.0]`.

micro-Uchuu, in a far smaller box, already reaches **270.3** — the z-component for the box's single largest halo (Mvir = 14039.99 in 1e10 M⊙/h, Len = 432000), 1 of 3,559,002 Spin values, extremes −84.2 to 270.3. Phase 5 widened the three micro-Uchuu Consistent-Trees packages to `[-1000.0, 1000.0]` (commit `23eab57e`, ~3.7× the observed maximum). **`uchuu` was deliberately left alone** because its correct bound is a separate scientific call.

A production-volume box will host far more massive clusters than micro-Uchuu, so `[-20, 20]` will fail. Decide the bound for `uchuu` and for the new `shin-uchuu` package before either is run at scale.

**Decided 2026-08-13 (joint review D6/D7): `uchuu` → `[-5000, 5000]`; `shin-uchuu` → `[-1000, 1000]` when the package is created.**

The principle first, since it outlives the numbers: a declared range exists to **catch corrupt or misconverted data**, not to encode physics, so it should be set generously and tightened from measurement.

**Where the range is actually enforced — corrected 2026-08-13.** An earlier draft of this section claimed a too-narrow bound "FATALs days into a one-shot production run". **That is wrong**, and it contradicted this section's own note below that range metadata reaches only `tests/generated/property_ranges.json`. There are **no references to property ranges anywhere under `src/`**: the range is emitted for `output: true` properties into that manifest and consumed by the test tiers (`tests/scientific/test_scientific.py`, `tests/integration/test_output_formats.py`). A too-narrow bound therefore fails **validation**, not the run; the production run itself is indifferent to it. That makes the asymmetry milder but still one-directional: a too-narrow bound is caught cheaply at the rehearsal and fixed by editing metadata, while a too-wide bound silently weakens a corruption signal on a run that cannot be repeated. Generous-then-tighten still holds; the urgency does not. Phase 5 set micro-Uchuu the same way (±1000 ≈ 3.7× the observed maximum).

The numbers are derived from one measured anchor and are **order-of-magnitude estimates to be confirmed at the rehearsal**, not measurements. `Spin` here is J/Mvir, and at fixed redshift specific angular momentum scales as j ∝ λ·Rvir·Vvir ∝ Mvir^(2/3); the maximum halo mass in a box is set by its volume, not its resolution. Anchoring on micro-Uchuu's measured 270.3 at Mvir = 1.4×10¹⁴ M⊙/h in a 100 Mpc/h box (`simulations/micro-uchuu/simulation_info.yaml`):

| Package | Box | Reasoning | Projected max | Bound |
|---|---|---|---|---|
| `shin-uchuu` | 140 Mpc/h | 2.7× micro-Uchuu's volume → modest rise in maximum halo mass. Its finer particle mass (**8.97×10⁵ M⊙/h**, confirmed 2026-08-14 — see §2.4; previously recorded here as 8.97×10⁴, low by 10×) multiplies *small* halos and does not raise the maximum, which the largest cluster sets | ≈350–450 | `[-1000, 1000]` (~2× headroom; also keeps all four ctrees packages declaring one bound for one quantity) |
| `uchuu` | 2000 Mpc/h | 8000× the volume, hosting clusters to ~2×10¹⁵ M⊙/h → (2×10¹⁵ / 1.4×10¹⁴)^(2/3) ≈ 5.9× | ≈1600 | `[-5000, 5000]` (~3× headroom; matches the `Vel [-5000, 5000]` / `VelDisp [0, 5000]` precedent already in these packages). **±1000 would be exceeded** |

**Two acknowledged weaknesses in this derivation**, both to be settled by measurement rather than argument. The ~2×10¹⁵ M⊙/h maximum assumed for `uchuu` is not sourced from any measurement in this repository. And the projection scales the *central* relation; it does not bound the scatter in spin at fixed mass (σ in ln λ is of order 0.4–0.5) or the extremal statistics over ~10¹¹ halos, either of which can push the true maximum above a 3× margin. The bounds above are therefore provisional: **measure the actual `Spin` extrema during the rehearsal and reset both bounds from data before the production run.** Also confirm the zero-mass carve-out there — halos with `Mvir == 0` deliberately retain raw `J` rather than `J/Mvir` (`src/io/tree/read_ctrees_ascii.c`, mirrored in `scripts/convert/fixups.py`), so they are not on the `Mvir^(2/3)` relation at all.

**Separately (joint review D8, closed 2026-08-14): the `dimensionless` units label on the ctrees packages' `Spin` was wrong metadata** — the quantity is J/Mvir in (Mpc/h)(km/s). Vision Principle 3 makes metadata the source of structural truth, and the vision statement asks that hidden assumptions be *harder* to introduce. It was reconciled as **its own slice with its own evidence run**, not folded into the range change: unlike ranges, units reach user-visible HDF5 `FieldMetadata` and the run-local `output_schema.json`. It landed before any Shin-Uchuu output existed, so no production file ever carried the wrong label. See `D8-SPIN-UNITS-RECONCILIATION-PLAN.md`.

**Corrected 2026-08-14 — `Spin` does NOT name two different quantities.** This section previously claimed that some packages store the Bullock parameter λ and others J/Mvir, and that the declared ranges track two different definitions. **That is wrong, and the correction enlarges D8's scope rather than shrinking it: every package stores J/Mvir, so every package's `dimensionless` label is wrong — not just the ctrees ones.**

Two independent lines of evidence, both checked directly:

- **The physics proves it.** `sage_set_disk_scale_radius.c:49` computes `lambda = spin_magnitude / (1.414 * vvir * rvir)`. For λ to be dimensionless, `Spin` must carry units of `Vvir × Rvir` = (Mpc/h)(km/s). λ is *derived from* `Spin`, never stored in it, and this is shared model code applied identically to every package.
- **The data proves it.** The tracked `mini-millennium` baseline (`tests/data/output/baseline/hdf5/model_000.hdf5`, `Snap063`) has `Spin` spanning **−18.23 to 14.49** with median |Spin| ≈ 0.083. A Bullock λ is order 0.03–0.05 and cannot reach ±18. The magnitudes are those of specific angular momentum for this box.

| Package(s) | Stored quantity | Declared range | Note |
|---|---|---|---|
| `micro-uchuu-{ascii,hdf5,snapshot}` | J/Mvir | `[-1000, 1000]` (widened 2026-08-12) | consistent with the measured 270.3 |
| `uchuu` | J/Mvir | `[-5000, 5000]` (set 2026-08-13 per D6; was `[-20, 20]`) | sized for this box's clusters |
| `micro-uchuu` | J/Mvir | `[-200, 200]` (`simulations/micro-uchuu/halo_properties.yaml:131`) | **Defect found 2026-08-14** — this is the *same micro-Uchuu data* in `lhalo_binary` form, and its ctrees siblings measure a maximum of **270.3**, so the declared bound is exceeded by its own data. Widen to `[-1000, 1000]` to match the siblings |
| `mini-uchuu`, `millennium`, `mini-millennium` | J/Mvir | `[-20, 20]` | plausible for these boxes, but no longer justified by a "λ is order 0.05" argument — confirm against measurement when convenient |

The descriptions were wrong too, not only the units: `millennium`, `mini-millennium`, `micro-uchuu` and `mini-uchuu` all read "Dimensionless spin parameter (Bullock definition)", which described λ rather than what the field holds. D8 corrected all eight packages' descriptions to name the specific angular momentum they actually store; none reads "Bullock" any more.

Note that range metadata reaches only `tests/generated/property_ranges.json` — it never enters `output_schema.json` or the HDF5 `FieldMetadata` table — so changing a range produces no output-schema or metadata delta.

#### D8 implementation scope, established 2026-08-14 — it is **not** a relabel

A scoping investigation was run before implementing D8. It is more than editing eight YAML strings, and the reason is that the units label is **not documentary — it drives code generation and unit conversion**:

- **`UNIT_REGISTRY` is a hard-coded dict in `scripts/generate_properties.py:132`**, and `_unit_info()` (`:331-334`) **raises `ValueError: Unknown unit label` for anything not in it**. `(Mpc/h)(km/s)` is not there, so simply writing it into the YAML fails `make generate` outright. Adding it is a **generator code change**, and the registry is shared by every property in every package.
- **`reference_units` in `src/core/core_properties.yaml` has only `mass`, `length`, `velocity` and `time`** — no specific-angular-momentum dimension. `time` shows the precedent for a compound entry (`derived_from: length/velocity`), so the shape exists; it has to be added deliberately, with its `in_cgs` (3.08568 × 10²⁴ × 1.0 × 10⁵ = 3.08568 × 10²⁹).
- **The h-convention is the trap, and it is the reason this needs an evidence run.** `Spin` declares no explicit `h_convention`, so it is *derived from the units label* (`_effective_h_convention()`, `:337-346`). Today `dimensionless` yields `h_convention: none`. A `(Mpc/h)(km/s)` label would naturally be **`carried`** — `Mpc/h` carries h, `km/s` does not. If that derivation changes, the generated conversion path can apply an h factor and **silently move every `Spin` value**, which would be a scientific change disguised as a metadata fix. The implementer must either pin `h_convention` explicitly to preserve today's behaviour, or add the reference dimension so source and target labels match and the factor cancels — and must **prove** the outcome, not assume it.

**Therefore the binding acceptance criterion is: `Spin` values byte-identical before and after; only the label, description and (for `micro-uchuu`) the range change.** That is exactly the "own slice with its own evidence run" D8 asked for, and it wants the bitwise tree-path preservation vehicle rather than a tiers-only pass.

**What is *not* a risk, checked directly:** the committed baselines are self-describing — each carries its own `metadata/output_schema.json` and is read through the schema written by the run that produced it, never a regenerated guess (`tests/data/README.md:23`). A units-label change therefore cannot invalidate them, and `assert_hdf5_schema_layout()` (`tests/framework/data_loader.py:354`) asserts group presence and format version, not units strings. No test asserts the string `dimensionless`.

**What this analysis missed:** it reasoned correctly about units strings but never considered that *refreshing the baseline files themselves* — which D8 Slice 2 (`f81e2385`) did, via the parent plan's mandated copy-only baseline procedure — would move `hdf5_format_version` out from under that same assertion. The refreshed baselines carry `1.2`; `assert_hdf5_schema_layout()`'s callers still asserted `1.1`, leaving the default integration suite red on the committed tree. `docs/dev/D8-FOLLOWUP-RECONCILIATION-PLAN.md` Slice 1 corrects the pinned expectation to `1.2`.

**Correction, 2026-08-14:** the paragraph above overstates the sidecar's role, and that overstatement has already cost time. It is true of the *binary* baseline, which is decoded through `metadata/output_schema.json` (`load_binary_halos` → `dtype_from_schema`). It is **not** true of the HDF5 baseline: `load_hdf5_halos` reads the file's own embedded compound dtype, so the HDF5 sidecar records provenance and never decodes anything. Reading it as a decode contract is what led a later commit to see a sidecar/data mismatch, conclude the HDF5 baseline was deliberately frozen at an old record layout, and write a "LAYOUT FREEZE" note asserting that. No such freeze ever existed — the data had been re-laid out to the current layout and only the sidecar lagged. `tests/data/README.md` now states the per-format distinction.

**Shape taken:** a small frozen implementation plan rather than a direct edit — not for size, but because the change touched shared generator machinery and carried a live path to silently altering numeric output. Its slices were: (1) extend the unit registry and reference dimension with the h-convention decision made explicit and proved byte-neutral (`612f83da`, `8746dc2c`, `c60b51b8`, `6267a1ab`); (2) relabel the eight packages and fix the wrong descriptions (`f81e2385`). Slice 1 was accepted; Slice 2 was implemented but stopped rather than accepted, because it left the default integration suite red for a reason outside that plan's frozen surface — see `D8-SPIN-UNITS-RECONCILIATION-PLAN.md` and `D8-FOLLOWUP-RECONCILIATION-PLAN.md`.

### 2.2 Recompute the driver's memory peak before the production run

The snapshot driver holds **two complete raw slabs unconditionally**, plus two processed generations, two galaxy pools, and the output and HDF5 buffers. **There is no memory-projection fallback branch in the code** — Phase 5 deliberately built none.

`SHIN-UCHUU-CONVERSION-PLAN.md`'s "Delegated obligation" subsection already carries this obligation, including the 85%-of-RAM trigger sentence: if the peak exceeds ~85% of installed RAM (≈435 GB on a 512 GB machine), replace the retained *previous* raw slab with a compact `{int32_t Len, int32_t NextProgenitor}` projection. Four corrections/additions to that text, the first two carried from the original recompute obligation and the last two from the joint review (`POST-PHASE-5-JOINT-REVIEW.md` F-5, F-9):

- Its `struct RawHalo` figure of **104 B is the default pair's** (mini-millennium/sage16). The ctrees-bridge catalog that Shin-Uchuu will use measures **88 B**, so the full-second-slab cost is ≈315e6 × 88 B ≈ **27.7 GB**, not ≈32.8 GB. Re-derive from the `shin-uchuu` package's own `sizeof(struct RawHalo)`, not from either recorded number.
- Neither the two processed generations nor the two galaxy pools are quantified anywhere; the plan's only numeric memory tables are converter-side.
- **The snapshot driver's output buffer is seeded at slab size, and `memset` makes the full seed capacity resident immediately** (`snapshot_acquire_generation()`, `src/core/snapshot_driver.c`): the buffer is seeded at `nhalos + MIN_HALO_ARRAY_GROWTH` records — ~315M × 176 B ≈ 55.4 GB at a z=0 Shin-Uchuu slab (the joint review's F-5 said 264 B; see the correction below) — and the seeding `memset` touches all of it immediately — do not remove the `memset` (it is defensive zeroing the marshaller is contractually expected to overwrite), but a `sizeof`-based projection that assumes lazy residency will underestimate the true peak by this amount. The recompute must count full seed capacity as resident, not just the bytes the marshaller happens to fill.
- **The reader's two staging buffers total ~10 GB transient at a 315M slab and are held simultaneously during a slab fill** (`snapshot_h5_fill_halos()`, `src/io/snapshot/read_snapshot_hdf5.c`): both buffers are allocated at the widest element size (8 B) and held together for the whole fill; every multi-dim catalog field in every current package is float32, so this is free headroom for the recompute rather than a feasibility item on its own, but it must be counted, not silently assumed away.

Two briefs rested on a superseded estimate until this recompute: `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` ("~300–450 GB peak estimated") and `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md` ("fits comfortably"). `MIMIC-DUAL-DRIVER-PLAN.md` already retired that number for omitting the retained previous slab.

#### Recompute 2026-08-13 — clear for the `sage16` production configuration under measured ratios, but the pool high-water is unmeasured and gates the decision

Struct sizes measured directly, not estimated: a read-only probe compiled against the generated headers for `MODEL=sage16 SIMULATION=micro-uchuu-snapshot` (the ctrees-bridge catalog Shin-Uchuu will use, with the production model) reports **`RawHalo` 88 B, `Halo` 176 B, `GalaxyData` 176 B, `HaloOutput` 264 B**. The 88 B confirms the correction above against the default pair's 104 B. Re-measure once `simulations/shin-uchuu/` exists; the frozen 16-column `/halos` contract means 88 B is the expected value.

**Correction to F-5 and to the bullet above — the output buffer is 176 B/record, not 264 B.** `struct OutputBuffer` holds `struct Halo *halos` (`src/core/output_buffer.h`), and `snapshot_acquire_generation()` allocates and `memset`s it with `sizeof(struct Halo)`. The 264 B figure is `struct HaloOutput`, the *on-disk output record*, which is a different structure and is not what this buffer holds. The joint review's "~315M × 264 B ≈ 78 GiB" therefore overstates this term by 1.5× (≈27.7 GB per generation). The residency point itself stands: the `memset` does make the full seed capacity resident, and it must be counted as such.

Per live generation, at the projected z=0 slab of N = 315,004,242 halos (GB = 10⁹ B):

| Term | Structure | B/halo | Total |
|---|---|---|---|
| Raw slab halos | `RawHalo` | 88 | 27.72 GB |
| Slab identity arrays | 2 × `int64_t` (`forest_index`, `halo_rank_in_forest`) | 16 | 5.04 GB |
| Aux array | `SnapshotHaloAux` (2 × `int64_t`) | 16 | 5.04 GB |
| Processed output buffer | `struct Halo`, seeded at `nhalos + MIN_HALO_ARRAY_GROWTH`, fully resident via `memset` | 176 | 55.44 GB |
| Galaxy pool | `GalaxyData`, at G ≈ N | 176 | 55.44 GB |
| **Per generation** | | **472** | **148.68 GB** |

- **Two live generations: ≈297.4 GB.** Both galaxy pools reach the largest slab's high-water mark, since `galaxy_pool_reset()` rewinds without freeing.
- **Reader staging transients: ≈10.08 GB** — `SNAPSHOT_H5_MAX_ELEMENT_SIZE` (8 B) × N × (1 + `NDIM`), both buffers held together for one slab fill (F-9). Counted on top of two complete generations, which slightly over-counts: staging is live during a slab *fill*, before that generation's processed buffer and pool are populated. Conservative in the safe direction.
- **Allowance for HDF5 write buffers, module allocations and allocator overhead: ~10 GB.** The HDF5 write buffer itself is small and bounded (8,192 `HaloOutput` records per requested snapshot, ≈0.15 GB at 70 outputs).

**Correction: the two 176 B terms are not both fixed at N.** The table's `C = G = N` is a *case*, not an invariant, and the first draft of this recompute wrongly asserted it was conservative. The processed buffer is only *seeded* at `nhalos + MIN_HALO_ARRAY_GROWTH` and grows ×1.5 whenever the output population exceeds capacity (`src/core/output_buffer.c`); and the galaxy pool allocates for every inherited progenitor galaxy and every newly initialized halo (`src/core/inheritance.c`), including Type 3 galaxies that are never emitted — so the output count does not bound the pool high-water either. Write the projection parametrically instead, per generation:

> **120·N + 176·C + 176·G**, where `C` is the processed buffer's realised capacity and `G` the pool's allocation high-water.

**Measured on micro-Uchuu** (slab counts read from the dataset's `n_halos`; output counts from the identity gate):

| Snapshot | slab `N` | `halos-only` output | ratio | `sage16` output | ratio |
|---|---|---|---|---|---|
| 023 | 608,076 | 822,183 | 1.35× | 603,671 | 0.99× |
| 028 | 620,478 | 937,126 | 1.51× | 615,207 | 0.99× |
| 049 (z=0) | 561,266 | 1,186,334 | **2.11×** | 557,127 | 0.99× |

The two models diverge sharply: `halos-only` keeps emitting orphans it never resolves, reaching 2.11× the slab at z=0, while `sage16` resolves mergers and disruption and sits at a stable **0.99×** across every snapshot — just under the seed, so its processed buffer never grows. Sensitivity at Shin-Uchuu scale:

| `C`, `G` | Peak | vs 435 GB trigger |
|---|---|---|
| 1.0N, 1.0N (measured `sage16` output) | ≈317 GB | clear by ~118 GB |
| 1.0N, 1.5N | ≈373 GB | clear |
| 1.5N, 1.5N | ≈428 GB | marginal |
| 2.25N, 2.11N (`halos-only`-like) | ≈579 GB | **over installed RAM** |

**Conclusion.** For the production configuration — `sage16`, which is what the conversion plan's Definition of Done requires — the measured output ratio is 0.99×N, giving ≈317 GB and no need for the compact previous-slab projection. But that rests on `G ≈ N`, and **`G` has not been measured**: nothing in the run reports the pool's allocation high-water, and Type 3 galaxies are allocated without being output. The trigger is crossed once `C` and `G` both approach ~1.55×N. **This item is therefore not closed on projection alone.**

**The binding gate is peak process RSS measured at the subset rehearsal** (§6 item 6), recorded alongside `C` and `G`. RSS rather than a reconstructed sum, because the analytic terms above provably do not capture everything:

- **Growth is by `realloc`, so the old and new buffers can briefly coexist.** `myrealloc_cat` subtracts the old allocation before calling libc `realloc` and records only the new one (`src/util/memory.c`), so the allocator's own high-water counter cannot see the transient when the block has to move. Growing an N-sized output buffer to 1.5N at Shin-Uchuu scale means a 55.44 GB old block alongside an 83.16 GB new one — enough to lift the marginal `C=G=1.5N` case from ≈428 GB to roughly 456–484 GB, i.e. across the trigger.
- Monotonic workspace, progenitor and segment scratch persist for the run and are not in the table.
- Pool chunk slack is real but small: chunks double to a `GALAXY_POOL_MAX_CHUNK` cap of 2²², so at 315M galaxies the unused tail is under one chunk per pool (≈1% of `G`).

Enumerating each of these analytically would add arithmetic without adding confidence; a single RSS number subsumes all of them, including allocator overhead. Measure RSS, and treat the parametric table as the thing RSS is checked against. Note the F-5 correction cuts both ways: at the erroneous 264 B even the `sage16` case would read ≈373 GB.

#### Instrumentation landed 2026-08-19 — road step 0 closed

`C`, `G`, the output population `P` (§2.3's term) and peak process RSS are now instrumented, ahead of the rehearsal that consumes them, because the instrumentation is fixture-scale work and only the *measurement* needs Shin-Uchuu. Reported at run end by `print_run_memory_profile()` (`src/util/run_profile.h`): the output buffer's realised capacity (`C`), the population it actually reached (`P`), the galaxy pool's allocation high-water (`G`) with its resident slot count and chunk slack, and peak process RSS from `getrusage(RUSAGE_SELF).ru_maxrss` — bytes on macOS, kilobytes on Linux, so the conversion is explicit. Each term is collected from every contributing buffer and pool and reported as **one run-level maximum**, which is the per-generation upper bound this projection multiplies; per-contributor values are not printed. **Sizes are reported in GB = 1e9 B to match this section, whereas the tracked allocator's own reports use MB = 1024² — convert before comparing.**

**First measurements, and one caveat worth carrying to the rehearsal.** Fixture-scale validation on both drivers, 2026-08-19:

| Run | `C` | `P` | `G` | Peak RSS |
|---|---|---|---|---|
| `sage16` / `mini-millennium`, tree-ordered | 74,345 | 14,648 | 15,525 | 0.065 GB |
| `halos-only` / `micro-uchuu-snapshot`, snapshot-ordered, all 50 snapshots of the real dataset | 1,398,325 | — | 1,186,334 | 2.593 GB |

Three things this already establishes, none of which the projection could assume:

- **`C` and `P` genuinely differ**, by ~5× on the tree path — that is the `MAXHALOFAC` seed, and it is why the §2.3 ceiling check must use `P` and the §2.2 residency term must use `C`. Conflating them misstates one or the other.
- **`G` can exceed `P`** (15,525 vs 14,648 above), confirming this section's point that Type 3 galaxies are allocated without being emitted, so the output count does not bound the pool.
- **Measured RSS is far above the analytic terms.** In the snapshot run the per-generation analytic terms come to well under half the 2.593 GB measured, so the unmodelled remainder is large in absolute terms even at fixture scale. **This is corroboration of this section's decision to make RSS the binding gate, not a Shin-Uchuu projection:** the run was `halos-only` (whose `GalaxyData` is a 1-byte placeholder, so its `G` term is negligible and unrepresentative) and `halos-only` is also the configuration whose orphan accumulation reaches 2.11×N. Do not extrapolate this ratio to `sage16` at Shin-Uchuu scale; measure it.

The instrumentation this depends on landed 2026-08-19 as `src/util/run_profile.{c,h}`; see `MIMIC-DEVELOPMENT-PATHWAY.md` → "Completed Work".

#### Measured at the Shin-Uchuu rehearsal, 2026-08-26 — the trigger fires, and two corrections to this section

This is the measurement this section was waiting for. Two `sage16` snapshot-ordered runs, identical in model, driver, `SubSteps: 10` and requested output-snapshot count, differing only in slab scale:

| Run | largest slab `N` | peak RSS | `C` | `P` | `G` | pool allocated (slack) |
|---|---|---|---|---|---|---|
| `sage16` / `micro-uchuu-snapshot` | 621,360 | **2.184 GB** | 622,360 | 616,114 | 636,789 | 1,040,384 (38.8%) |
| `sage16` / `shin-uchuu` (rehearsal subset) | 9,006,294 | **14.301 GB** | 9,007,294 | 8,979,389 | 9,210,010 | 12,574,720 (26.8%) |

Ratios are strikingly stable across a 14.5× scale change: `C`/N = 1.00161 → 1.00011, `G`/N = 1.02483 → 1.02262, `P`/N = 0.99156 → 0.99701. **`C` = N + 1000 exactly at the larger scale** — the buffer is seeded at `nhalos + MIN_HALO_ARRAY_GROWTH` and never grew, so the `sage16` production configuration does not exercise the geometric-growth path at all.

**Correction 1 — the `176·G` term must use the pool's *allocated* slots, not its high-water `G`.** This section's own note that "pool chunk slack is real but small: ... at 315M galaxies the unused tail is under one chunk per pool (≈1% of `G`)" is right at production scale and badly wrong at rehearsal scale, where slack is 27–39%. The fit is therefore written with the pool separated:

> **RSS = 1.221 GB + 960.9 B/halo × N + 2 × 176 B × pool_allocated(N)**

reproducing both measured points exactly (2.184 GB and 14.301 GB). Because slack shrinks with scale, the *effective total* slope falls from 1,550 B/halo at micro-Uchuu to 1,452 at the rehearsal and 1,322 at production; a single naive slope through the two points therefore overstates the production peak.

**The unmodelled residency this section predicts is real, and is ~35% of the non-pool slope.** Like for like, the parametric non-pool part is 2·(120 + 176·`C`/N) + 32 = **624.0 B/halo** against the fitted **960.9**, i.e. **336.8 B/halo unaccounted for**. That is the term this section deliberately declines to enumerate analytically, and it is why measured RSS is the gate rather than the parametric sum. **The two projections do not differ by exactly that residual**, because the parametric form carries a flat ~10 GB allowance while the fit carries a 1.221 GB intercept: 336.8 B/halo x N is 106.1 / 119.4 GB at the two production slabs, against table gaps of 97.6 / 111.5 GB - the ~8.7 GB difference being precisely those two constants. Do not substitute one projection for the other.

**Correction 2 — the peak is set by the largest slab, not the z=0 slab.** Every projection in this section uses N = 315,004,242, the box's z=0 halo count. The rehearsal measures the largest slab at **snapshot 44, 1.1258× its own z=0 slab**, and the peak is a **broad plateau** — snapshots 41–48 all within 1% of each other — so both live generations sit at it rather than one being much smaller. The projected production largest slab is therefore **N ≈ 3.546 × 10⁸**.

| Projection at | parametric form | measured-RSS fit (**the one to use**) | vs the 435 GB trigger |
|---|---|---|---|
| z=0 slab, N = 315,004,242 | 320.0 GB | **417.6 GB** | clear by 17.4 GB |
| largest slab, N ≈ 354,618,825 | 358.9 GB | **470.4 GB** | **OVER by 35.4 GB** |

**The trigger is crossed by the point forecast, but the compact previous-slab projection is deferred rather than required (owner decision, 2026-08-26).** It does not cover the gap anyway: replacing the retained previous generation's 88 B `RawHalo` with `{int32_t Len, int32_t NextProgenitor}` saves 80 B/halo, i.e. 28.4 GB at N ~ 3.546e8, taking 470.4 GB to ~442.0 GB - still above 435 GB; also releasing that generation's two int64 identity arrays and its `SnapshotHaloAux` (32 B/halo, 11.3 GB) reaches ~430.7 GB, under by only 1%.

**A units note that matters here, because it makes the recorded trigger more conservative than it reads.** The host reports `hw.memsize` = 549,755,813,888 B, i.e. exactly **512 GiB** — the capacity Apple markets as "512 GB". This section reports in **GB = 10⁹ B**, so that same memory is **549.76 GB** in projection units. The recorded trigger of "~435 GB (85% of 512 GB)" was formed as 512 × 0.85 and then read on the 10⁹-byte scale, so it is really **79% of capacity**; a literal 85% would be **467.3 GB**. No extra memory is being claimed — only the same memory counted consistently.

**And the trigger's headroom assumption does not match the conversion host.** 435 GB was set as 85% of a 512 GB machine, reserving 15% for everything else. Measured on that host with a normal desktop session live, committed memory outside Mimic is **~37.7 GB** (31.0 GB anonymous + 6.7 GB wired); the 170.8 GB of file-backed pages is evictable cache, not competition. On the otherwise-idle box these runs already require, Mimic has **~540 GB**, so 470.4 GB is ~87% utilisation with ~70 GB spare.

**The response is therefore: run lean, and re-project from the production conversion report's own per-snapshot slab counts before committing to the run** - that report supplies the real largest slab and removes this projection's one unavoidable extrapolation. The compact-slab change stays specified and available if the re-projection comes in materially higher.

**What this does not cover, and should be closed cheaply anyway:** `sage16` never grows its output buffer (`C` = N + `MIN_HALO_ARRAY_GROWTH` exactly) and measured `P`/N is 0.99701, i.e. 0.3% below the growth threshold. Crossing it steps `C`/N to 1.5 - about 55 GB more resident plus a `realloc` transient holding both blocks - a step no amount of free RAM absorbs. Seeding the buffer with a few percent headroom above `nhalos` rather than a flat +1000 costs ~2-3 GB at production and removes the cliff. Note also that the recorded "435 GB = 85% of 512 GB" is conservative in its own terms: the machine holds 549.76 × 10⁹ B, so 435 GB is 79% of installed and a literal 85% would be 467.3 GB — which 470.4 GB still exceeds. The z=0 reading is not a safe fallback: clearing by 4% on a 35× extrapolation leaves no headroom for the `realloc` transient described above, where an N-sized buffer growing to 1.5N briefly holds both blocks.

#### Seed headroom landed 2026-08-26 — JR §6 item 11 closed, and a measurement caveat that matters more

**The cliff is gone.** `snapshot_acquire_generation()` now seeds the output buffer at `nhalos + max(SNAPSHOT_OUTPUT_SEED_HEADROOM x nhalos, MIN_HALO_ARRAY_GROWTH)` with the fraction at 5%; the growth policy is untouched and the flat increment remains the floor for small slabs. Because `C` is a reported quantity the cost needs no fitting: it moves 9,007,294 -> 9,456,608 records at the rehearsal slab (+79.1 MB per generation, +158 MB for both) and 622,360 -> 652,428 at micro-Uchuu. At the projected production largest slab the headroom is 17,730,941 records = **3.12 GB per generation, 6.24 GB for both**, moving the parametric non-pool slope from 624.0 to **641.6 B/halo** and the largest-slab projection from 470.4 GB to **~476.6 GB**. Emitted output is bitwise unchanged across all four snapshot-ordered configurations, proved by running HEAD and the changed tree back to back rather than by argument.

> **A caveat this section needs, because it undermines any single-run RSS figure.** Measuring the change at Shin-Uchuu scale produced, for the *same* binary and dataset: baseline **34.445 GB** then **16.752 GB**; changed **17.527 GB** then **14.685 GB**. The changed build reads *lower* than the baseline, which a larger buffer cannot cause. `/usr/bin/time -l` confirms the 34.445 GB byte-for-byte with `swaps 0`, so the peak value is real, nothing is paging, and the instrument is sound. **The cause is a hypothesis, not a measurement:** the pattern is consistent with the first run of each set paying a cold page cache on the 41 GB dataset and macOS charging those file-backed pages to the process footprint, but nothing here attributes the extra residency to file cache rather than to genuine working set. Treat the highest observed figure as the conservative bound and do not discount it on the strength of this explanation. **Run-to-run variance is therefore ~17.7 GB at this scale, two orders of magnitude larger than the 158 MB the seed change actually costs.** The 14.301 GB anchor recorded above is the warm steady state and is reproduced by the warm runs here; it is not contradicted. But it does mean: take memory measurements warm, repeat them, and never read a single cold run as the peak. For the production run this is sharper still — the dataset is 2.31 TB and can never be warm. Re-project from warm, repeated runs where the dataset permits it, and where it does not, plan against the **highest** observed figure rather than assuming the excess is cache.

**Why this fit is used for `sage16` and must not be reused for `halos-only`.** The identical method applied to `halos-only` (2.593 GB at N = 621,360 from the 2026-08-19 measurement above, 34.926 GB at N = 9,006,294) fits a slope of 3,851 B/halo against a parametric 1,068 - under-predicting by 3.6x, where `sage16` matches to 0.98x. Growth is a large part of that: `sage16` holds `C` = N + `MIN_HALO_ARRAY_GROWTH` exactly and never grows its output buffer, while `halos-only` reaches `C`/N = 2.25, and the last x1.5 step briefly holds a 1.5N block beside a 2.25N one (about 1,320 B/halo over two generations) that `myrealloc_cat` cannot see. **It is not the whole story** - that accounts for roughly half the 2,783 B/halo gap - so treat it as the identified mechanism, not a complete decomposition. **The corollary is a fragility worth carrying:** measured `P`/N is 0.99701 and 0.99156, so the production configuration sits within ~0.3% of the point where growth triggers, after which `C`/N jumps to 1.5 and the hidden transient appears.

**How much confidence 470.4 GB deserves.** It is a **point forecast with real model uncertainty**, not a measured production peak: the two anchor runs share model, driver, `SubSteps` and output-snapshot count but come from **different simulations with different merger histories**, the extrapolation is 39x beyond the larger point, and the largest-slab step assumes the production box's largest/z=0 ratio matches the subset's 1.1258 - which the excluded super-forest could move. The output-count control rules out accumulation with requested outputs; it does not prove linearity in N. **The trigger decision is therefore a conservative risk decision on a point forecast, not a proof.**

**A control worth recording, because it rules out an obvious alternative explanation.** Varying the requested output-snapshot count on the small run — 1, 8 and 50 snapshots — left peak RSS flat at 2.182 / 2.184 / 1.959 GB. Residency scales with the slab, not with output volume, which is what licenses extrapolating in N and also means the writer's per-snapshot rescan (F-3 / D3) costs time rather than memory.

#### Platform audit 2026-08-20 — the instrument is scale- and platform-sound, and the RSS branch is now measured on the target host

The instrumentation above was developed and validated entirely on macOS at fixture scale, which raises a fair question: could an instrument built that way misreport at Shin-Uchuu scale, on the machine the rehearsal actually runs on? That failure would be expensive in exactly the way this item exists to prevent — discovered only after a multi-week run. Audited on 2026-08-20; three findings, no defect.

- **Integer widths are sound at the projected scale.** `C`, `P`, `G`, `MaxProcessedHalos`, `OutputBuffer.count`/`capacity` and the pool's `live`/`live_high_water`/`slots_allocated` are all `int64_t`. `chunk_count` is an `int`, but `GALAXY_POOL_MAX_CHUNK` caps a chunk at 2²² galaxies, so the ~315M-galaxy generation this projection contemplates needs fewer than 200 chunks. `galaxy_pool_create()` takes an `int` capacity, but both production call sites (`src/core/main.c:365`, `src/core/snapshot_driver.c:861`) pass `0`.
- **The slab has a hard `INT_MAX` ceiling, already guarded.** `snapshot_driver.c:674` FATALs when `slab.nhalos > INT_MAX`, because the member loop narrows the count to `int`. The projected z=0 slab is ~6.7× clear of it and the guard is loud rather than silent, but the rehearsal should confirm the real per-snapshot slab sizes against it — this is a second ceiling alongside §2.3's two, and it applies to the *slab* `N` rather than to the output population `P`.
- **`ru_maxrss` units differ by platform, and the Linux branch had never been exercised.** `run_profile_peak_rss_bytes()` returns `ru_maxrss` unscaled on macOS (bytes) and multiplied by 1024 elsewhere (kilobytes). Every recorded measurement, including the byte-exact `/usr/bin/time -l` cross-check, was taken on macOS — yet **peak RSS is this section's binding gate** and the rehearsal runs on Linux. Had the scaling been wrong, the single number the memory decision rests on would have been off by 1024×, and only after the run. **Measured on the conversion host** (`tooarrana1`, RHEL 9, kernel 5.14.0, gcc 11.5.0): a probe touching 800 MiB reported `ru_maxrss` = 820,256 against `/proc/self/status` `VmHWM` = 820,480 kB and `/usr/bin/time -v` = 820,156 kB. **Kilobytes confirmed; the `× 1024` branch is correct on the target hardware**, now by measurement rather than by convention.

#### Host decision 2026-08-25 — the rehearsal runs on macOS, so the bytes branch is the live one

The audit above was written expecting the rehearsal to run on Linux, and validated the `× 1024` kilobytes branch on `tooarrana1` for that reason. **That expectation is superseded: every step that needs memory or CPU — the conversion, every Mimic run, and all subset planning — happens on the Mac Studio** (M3 Ultra, 32 cores, 512 GB); only two streaming, sub-1 GB subset steps run remotely. `tooarrana1` was measured on 2026-08-25 at **251 GB RAM and 4 cores** — roughly half what the rank pass and the snapshot driver need at production scale — so it is a source and streaming-extraction host only. `SHIN-UCHUU-CONVERSION-PLAN.md` → "Where The Work Runs" owns this decision and its storage consequences.

For this section that means `run_profile_peak_rss_bytes()` takes the **bytes** branch of `ru_maxrss`, which is the branch every recorded measurement here already used and the one cross-checked byte-exact against `/usr/bin/time -l`. The Linux confirmation is not wasted — it remains the evidence that the instrument is platform-sound, and it becomes load-bearing again if any part of this work later moves to a cluster (`MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`). Nothing about the binding gate changes: **peak process RSS measured at the rehearsal**, now on macOS.

**What this does not settle.** The audit covers the *instrument*, not the *values*. The fixture-scale numbers in the table above remain validation evidence and nothing more, and the caveat against extrapolating them stands unchanged: §6 items 2, 3 and 9 close against the rehearsal's own measurements.

### 2.3 Two output-path ceilings below the projected z=0 population — check against the lower one first

**First ceiling (record correction, joint review F-2): `MAX_HALO_ARRAY_SIZE = 1000000000`** (`src/include/constants.h:43`), enforced in output-buffer growth (`marshal_workspace_to_output_buffer()`, `src/core/output_buffer.c`). Output-buffer growth is clamped to this constant and FATALs when it cannot grow past it. A snapshot whose output population (slab galaxies + accumulated orphans) exceeds 10⁹ aborts in the marshaller **before** the second ceiling below is ever consulted — at roughly half the headroom this section previously assumed. The constant was not revisited when `OutputBuffer` widened to int64, and the fatal's message presents it as a structural invariant rather than a tunable (its `%d` on what would become an `int64_t` also needs `PRId64` if this is ever widened). **The projected-population check must compare against 10⁹, not 2.1e9; if widening is ever needed, both ceilings move together.**

**Second ceiling: `output_increment_halo_counters_checked()` still caps at `INT_MAX`.** `src/io/output/util.c` FATALs when `TotHalosPerSnap[snap_index] == INT_MAX` (both branches), even though Phase 5 widened the schema to int64. Unreachable at micro-Uchuu scale — the largest per-snapshot total is ~1.19M against a 2.1e9 cap — and it was deliberately left unfixed because relaxing it changes an error contract no Phase 5 slice specified. This ceiling only matters once the first one above it is cleared.

**At Shin-Uchuu scale this needs a decision.** The projected ~315M z=0 slab sits well under 10⁹, so the first ceiling fires only if the output population exceeds the slab by ~3× (orphan accumulation would have to be extreme) — this is a record correction, not a projected abort. Still, check the projected z=0 output population against **10⁹** before running; if it is within an order of magnitude of either ceiling, widen the counter and its guard to int64 first, moving both ceilings together.

**Check performed 2026-08-13 — clear for the production configuration, but expressed in the output population, not the slab count.** The ceilings apply to the **output population `P`** (slab galaxies plus accumulated orphans), not to the slab count `N`, so the headroom must be computed from `P`. Using the ratios measured in §2.2:

| Configuration | `P` at z=0 | vs 10⁹ (`MAX_HALO_ARRAY_SIZE`) | vs `INT_MAX` |
|---|---|---|---|
| `sage16` (0.99×N, the production model) | ≈312M | 3.21× clear | 6.9× clear |
| `halos-only` (2.11×N) | ≈665M | **1.50× clear** | 3.23× clear |

**Measured at the rehearsal, 2026-08-26 — clear, and now on a measured `P`/N rather than a carried-over one.** `sage16` snapshot-ordered gives `P`/N = **0.99701** at a 9,006,294 slab, reproducing micro-Uchuu's 0.99 at 15× the scale on a different simulation. At the projected production largest slab (N ≈ 3.546 × 10⁸, see §2.2's correction 2) that is P ≈ 3.536 × 10⁸: **2.83× clear** of `MAX_HALO_ARRAY_SIZE` = 10⁹ and **6.07× clear** of `INT_MAX`. The slab itself is **6.06× clear** of the `INT_MAX` slab guard at `snapshot_driver.c:674`. No widening is required. Note the `halos-only` configuration measures `P`/N = 1.908 on the same dataset, so the caution about configuration-dependence in this section stands — it is simply not the production model.

An earlier version of this note divided the ceilings by `N` and reported 3.17× / 6.8× unconditionally — the same `N`-versus-`P` conflation §2.2 corrects, and it overstated the margin for any configuration whose output exceeds its slab. The production configuration is genuinely clear, so **no widening is required now**; but since §2.2 also concludes that micro-Uchuu's 0.99× ratio may not carry to Shin-Uchuu's far finer resolution, **this check stays parametric in `P` and is confirmed by the same rehearsal measurement** rather than closed on the projection. If the measured `P` lands within an order of magnitude of 10⁹, widen the counter and its guard to int64 first, moving both ceilings together.

### 2.4 Confirm the Shin-Uchuu particle mass before freezing the package — CLOSED 2026-08-14

**The particle mass is 8.97 × 10⁵ Msun/h.** Confirmed from the simulation documentation this item asked for: Ishiyama et al. 2021, the Uchuu suite paper ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)), which states that Shin-Uchuu has "262 billion (6400³) particles in a box of side-length 140 Mpc/h, with particle mass 8.97 × 10⁵ M☉/h". The same paper gives Uchuu as 12800³ particles in 2.0 Gpc/h at 3.27 × 10⁸ M☉/h.

**This corrected a real error rather than merely confirming a guess.** `SHIN-UCHUU-CONVERSION-PLAN.md:34` recorded ~8.97 × 10⁴ Msun/h — the right mantissa with an exponent low by exactly a factor of 10. Cross-checked independently against the package's own declared cosmology before the paper was consulted: Ω_m ρ_crit L³ / N = 0.3089 × 2.77537 × 10¹¹ × 140³ / 6400³ = 8.97 × 10⁵ Msun/h. Both the paper and the arithmetic agree, and the erroneous value is not reproducible from any consistent choice of box size and particle count.

**Two derived claims rested on the wrong value and are corrected with it.** The `shin-uchuu` row in §2.1's `Spin` table described the particle mass as "far finer" than it is, and `POST-PHASE-5-JOINT-REVIEW.md:154` stated a "3,600× finer particle mass" relative to micro-Uchuu. The true ratio against micro-Uchuu's declared 3.25 × 10⁸ Msun/h is **≈360×, not ≈3,600×**. That matters for the still-open work: the low-mass population, and therefore the orphan statistics that set the output-buffer capacity `C` and the galaxy-pool high-water `G` in item 3, scales with the particle-mass ratio. The projection is an order of magnitude less severe than the superseded figure implied — a correction in the safe direction, but item 3's binding gate remains the measured peak RSS at the rehearsal.

**No package or code change follows.** `simulations/shin-uchuu/` does not exist yet; it is created by the conversion plan, which now carries the confirmed value.

**Third independent confirmation, 2026-08-20 — from the simulation's own parameter file.** The source data was located on the conversion host (see §2.2's platform note and `MIMIC-DEVELOPMENT-PATHWAY.md`), and `/fred/oz214/simulations/uchuu/shinuchuu/shinuchuu.par` declares `PartMass 0.0000897` in units of 10¹⁰ Msun/h — that is **8.97 × 10⁵ Msun/h**, matching both the paper and the arithmetic. The same file independently confirms the rest of the package metadata this item will need: `Omega 0.3089`, `OmegaLambda 0.6911`, `Hubble_h 0.6774`, `BoxSize 140.0`, `TreeType consistent_trees_ascii`, `LastSnapShotNr 69` (70 snapshots), and `NumSimulationTreeFiles 2744`. This is the strongest class of evidence available for the value — the producer's own configuration — and it closes any residual doubt that the recorded 8.97 × 10⁴ was a transcription error rather than a convention difference. The runtime guard is unaffected and still correct: the reader compares `particle_mass_msun_h`, `box_size_mpc_h` and the three cosmology values against `MimicConfig` **in every snapshot file** and aborts on mismatch (`snapshot_h5_check_physical_value()` calls in `open_run`, `src/io/snapshot/read_snapshot_hdf5.c`; mass compared as `PartMass × 1e10`). That guard catches a *disagreeing* pair; it cannot catch an agreeing pair of wrong values, which is precisely why this item existed and why the value now carries a citation rather than an inference.

### 2.5 Calibrate the remaining property ranges for `shin-uchuu`

`SHIN-UCHUU-CONVERSION-PLAN.md:439` lists `deltaMvir`, `Len` (floor is 1 at this resolution) and `Spin` as requiring calibration from a test run. For reference, the micro-Uchuu packages now ship `Spin [-1000, 1000]`, `Vel [-5000, 5000]`, `VelDisp [0, 5000]`, `Vmax [10, 5000]`.

**Calibrated at the rehearsal, 2026-08-26.** Measured over the whole converted subset (all 70 snapshots, 406,668,896 halos, zero non-finite values): `Spin` **[−11.673591, +17.797567]**, `Len` **[2, 13,575,251]**, `Pos` **[0.0, 140.0]** (touching both declared bounds; the check is inclusive so it passes), `Vel` [−2198.26, +2782.30], `VelDisp` [0.0, 465.57], **`Vmax` [1.77, 407.38]**. In output, `deltaMvir` is [−58.55, +78.72] for `sage16` and [−205.30, +78.72] for `halos-only`.

**Both surviving bounds are anchored against micro-Uchuu rather than against the subset alone, because the subset's margins flatter them.** micro-Uchuu reaches `Spin` 270.30 and `deltaMvir` [−2409, +3860] at a maximum halo mass of 3.09 × 10¹⁴ Msun/h, against the subset's 1.2177 × 10¹³. Scaling to the production box's ≥1.2288 × 10¹⁵ (3.98× micro-Uchuu) gives `Spin` ≈ 270.30 × 3.98^(2/3) ≈ **680** against the 1000 bound, and `deltaMvir` plausibly **1.5–2 × 10⁴** against ±20000. Both are inside, neither comfortably. **The two need different gates, and only one can be a pre-run scan.** `Spin` is present in the converted input dataset, so it is scannable before the run - a binding pre-run gate. `deltaMvir` is created during inheritance (`src/core/inheritance.c`) and exists only in Mimic output, so it cannot be scanned in advance: it is a **binding post-run range validation** on the production run's own output.

Outcome: `Spin` and `deltaMvir` left unchanged — neither is *confirmed*, both are **not refuted by the rehearsal**, which is why both carry a production gate; no `Len` range added (output `Len` is legitimately 0 for orphans); and **`Vmax` corrected from `[10.0, 5000.0]` to `[0.0, 5000.0]` in both shin-uchuu packages** — the micro-Uchuu floor is violated by Shin-Uchuu's 362×-finer particle mass, which resolves halos down to 1.77 km/s at `Len` = 2. That was a live failure of `test_physical_ranges`, not a theoretical one. See `POST-PHASE-5-JOINT-REVIEW.md` §6 items 2 and 9.

**Clarification (joint review F-10):** `deltaMvir` is not a package catalog range like `Len`/`Spin` — it is a **core-level output property** (`src/core/core_properties.yaml`, range `[-20000.0, 20000.0]`, already annotated "symmetric wide bounds to accommodate Uchuu-scale mass swings"), computed during inheritance rather than read from any simulation package's `halo_properties.yaml`. Calibrating it is legitimate pre-Shin-Uchuu work — the range check still needs confirming against a Shin-Uchuu test run — but it is checked and edited in `core_properties.yaml`, not in `simulations/shin-uchuu/halo_properties.yaml`.

### 2.6 The identity encoding's cross-reader equivalence has no unit test

The gate's correctness rests on the snapshot reader's `forest_index` / `halo_rank_in_forest` matching the tree reader's `HaloRankInForest` and `GlobalForestOffset + unit` for the same physical halo. **This is validated only by the gate itself** — no unit test pins it. It has now held bitwise across two models, two timestep schemes and 7.5M+ galaxy records on micro-Uchuu, which is strong empirical evidence, but the equivalence is a property of the *converter and reader pair* and Shin-Uchuu is a new conversion.

**Run the identity gate on a Shin-Uchuu subset before trusting a full production run.** That is the cheapest possible check against a conversion-side indexing error.

### 2.7 The Uchuu-family packages declare a particle mass that is 0.6% low — NEW 2026-08-14

**All six Uchuu-family packages declare `particle_mass: 0.0325` (1e10 Msun/h) = 3.25 × 10⁸ Msun/h. The physically consistent value is 3.27 × 10⁸ Msun/h.** Found while confirming the *Shin*-Uchuu mass for §2.4; that item is closed and correct, this is a separate, newly-discovered defect in the shipped packages.

Affected: `uchuu`, `mini-uchuu`, `micro-uchuu`, `micro-uchuu-ascii`, `micro-uchuu-hdf5`, `micro-uchuu-snapshot`.

**Evidence.** Ishiyama et al. 2021 ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)) gives Uchuu as 12800³ particles in 2.0 Gpc/h at **3.27 × 10⁸ M☉/h**; the suite shares mass resolution across box sizes. The arithmetic agrees and is self-checking: for micro-Uchuu's 100 Mpc/h box, Ω_m ρ_crit L³ / N with Ω_m = 0.3089 gives exactly 3.2704 × 10⁸ at **640³** particles, whereas the declared 3.25 × 10⁸ implies **≈641.3³** — not an integer particle count, which is the tell that it is a transcription error rather than a different convention. (Both figures use ρ_crit = 2.77537 × 10¹¹ h² Msun/Mpc³. The cube root was recorded as 641.6³ until 2026-08-20; it is 641.31–641.34 across every reasonable choice of ρ_crit, so the argument is unchanged and only the digit was wrong.)

**This is not a one-line config fix. Changing the YAML alone breaks every snapshot-format run.** Phase 5 added a physical-header agreement check: the reader compares each file's `particle_mass_msun_h` against `MimicConfig.PartMass × 1e10` and **aborts** on mismatch, with a rounding tolerance of 16 × DBL_EPSILON (≈ 1.2 × 10⁻⁶ at this magnitude) against a difference of 2 × 10⁶ — twelve orders of magnitude outside. Both the committed test fixture (`simulations/micro-uchuu-snapshot/_tests/data/snapshot_*.h5`) and the real 50-file dataset stamp `particle_mass_msun_h = 325000000.0`, so both would abort at `open_run`, taking the **cross-format identity gate** and every snapshot-pair integration test with them.

**It also moves physics.** `src/core/virial.c:51` returns `Len × PartMass` for every halo that is not an FoF central with a valid `HaloMass` — that is the `else` branch for all subhalos, not a rare fallback. A 0.62% change in `PartMass` shifts every subhalo's virial mass, and therefore `Rvir`, `Vvir` and downstream galaxy properties, for all Uchuu-family runs. Separately, `Len` in snapshot-format data is computed by the converter as `round(Mvir_native × 1e-10 / PartMass)`, so regenerated data shifts by the same fraction and may move by a whole particle for small halos.

**What a correct fix requires**, in order: correct the six `simulation_info.yaml` files; regenerate the committed snapshot fixture; re-convert or re-stamp the 50-file real micro-Uchuu snapshot dataset so its headers agree; re-run the cross-format identity gate to re-certify (it is self-consistent, so it should pass once both sides agree, but the previously certified output values will have moved); and record the ~0.6% shift as an intended scientific change rather than a regression.

**Not a Shin-Uchuu blocker** — the Shin-Uchuu package will be created with the confirmed 8.97 × 10⁵ — but it is a correctness defect in six shipped packages and should be scheduled deliberately. This item carried one prerequisite, **confirm the micro-Uchuu and mini-Uchuu particle counts from Skies & Universes before implementing**, because the 640³ and 2560³ figures above were inferred from the box size and the suite's shared mass resolution rather than read from a source. **That prerequisite is discharged — see below.**

**Scheduled 2026-08-19, discharged 2026-08-20.** The count sourcing needed nothing but a literature check and was done as remote-safe work; the result is recorded immediately below. **The fix itself stays last**, for the reason above: re-stamping the fixture and the 50-file dataset takes the identity gate offline until both sides agree again.

#### Counts sourced 2026-08-20 — prerequisite discharged, and it confirms the defect

**Both counts are now read from a source rather than inferred.** Skies & Universes — the Uchuu collaboration's own data portal — tabulates the suite directly on its [Uchuu simulation page](https://skiesanduniverses.org/Simulations/Uchuu/):

| Simulation | Box (Mpc/h) | Particles | Particle mass (Msun/h) |
|---|---|---|---|
| Uchuu | 2000 | 12800³ = 2,097,152,000,000 | 3.27 × 10⁸ |
| mini-Uchuu | 400 | 2560³ = 16,777,216,000 | 3.27 × 10⁸ |
| micro-Uchuu | 100 | 640³ = 262,144,000 | 3.27 × 10⁸ |
| Shin-Uchuu | 140 | 6400³ = 262,144,000,000 | 8.97 × 10⁵ |

**The 640³ and 2560³ figures are confirmed twice, the second time by a statement independent of that table.** The portal's DarkMatterParticles page describes a micro-Uchuu snapshot as holding "262 million" particles and a mini-Uchuu snapshot "16.8 billion" — and 640³ = 262,144,000 while 2560³ = 16,777,216,000. Those are per-snapshot particle counts written for a different purpose, so they corroborate the table rather than restate it.

**The suite's shared mass resolution is now sourced, not assumed — which is what this item actually turned on.** §2.7's argument required that micro-, mini- and full Uchuu genuinely carry one mass resolution, and they do: the box sizes and the particle counts scale identically, 100 : 400 : 2000 and 640 : 2560 : 12800 both being 1 : 4 : 20. Skies & Universes states **3.27 × 10⁸ Msun/h for all three**, matching the derivation above and confirming that the declared `particle_mass: 0.0325` is wrong for every one of the six packages, not just for the one the arithmetic was worked on.

**Shin-Uchuu's 6400³ is reconfirmed in passing, and item 8 now has a third independent source.** The same portal gives Shin-Uchuu as 262 billion (6400³) particles at 8.97 × 10⁵ Msun/h. Separately, the simulation's own parameter file on the conversion host declares `PartMass 0.0000897` (×10¹⁰ Msun/h) — see §2.4.

**No package or code change follows, deliberately.** This closes the *prerequisite*, not the item: §6 item 10 stays last for the reason stated above, and the fix is still the full sequence in "What a correct fix requires".


---

## 3. Correctness and hygiene items (do not block Shin-Uchuu)

**§3.5 and §3.6 were scheduled 2026-08-19 and closed 2026-08-20** as the remote-safe hardening batch run while the Shin-Uchuu rehearsal is blocked (recorded in `MIMIC-DEVELOPMENT-PATHWAY.md` → "Completed Work"): they commit test coverage that today exists only as run evidence, hardening the identity gate before it is asked to certify a Shin-Uchuu subset. The rest of §3 stays opportunistic.

### 3.1 `make dump-ctrees-topology-tool` is broken — **reclassified 2026-08-13 (D10): this is a converter-scale-pass prerequisite, not hygiene**

The converter scale-engineering pass (§6 item 7) had as its acceptance gate the micro-Uchuu validation battery **plus the topology cross-check**, and the cross-check is driven by this tool (`crosscheck.py compare --reference-topology <dump>`). It therefore gated what was then the largest remaining item and could not be filed as non-blocking. Item 7 closed 2026-08-28, and this tool produced the dump its acceptance gate ran against.

**Fixed 2026-08-13.** `make MODEL=halos-only SIMULATION=micro-uchuu-ascii dump-ctrees-topology-tool` builds and links clean again, for the first time since `5cd28b94`. Both undefined symbols were resolved as this section suggested: `src/io/snapshot/registry.c` joins the harness's source list (compiled **without** `-DHDF5`, so the snapshot reader table is empty and no snapshot reader is pulled in — correct for a harness that reads tree-ordered input only, and it keeps the deliberately minimal dependency surface intact), and `narrow_int64_to_int_checked` moved from `src/core/output_buffer.c` to `src/util/numeric.c`, where a general numeric-narrowing helper belongs and which the harness already builds. Its declaration moved from `src/include/proto.h` to `src/util/numeric.h`, and its three unit tests and their fork helper moved from `test_output_buffer.c` to `test_numeric_utilities.c`. Of the four call sites, three newly gained the `numeric.h` include (`src/core/snapshot_driver.c`, `src/io/tree/interface.c`, `src/io/output/binary.c`); `src/core/build_model.c` already had it. Pure relocation: no behaviour change.

Broken since `5cd28b94`, which predates Phase 5, and Phase 5 added a second breakage. **Fixed 2026-08-13 — see the note below.** The link failure was:

```text
Undefined symbols for architecture arm64:
  "_narrow_int64_to_int_checked", referenced from: _load_unit in src_io_tree_interface.o
  "_snapshot_reader_lookup",      referenced from: _parse_input_section in src_core_read_parameter_file.o
```

Both are traceable:

- `snapshot_reader_lookup` is defined in `src/io/snapshot/registry.c`, which `tests/unit/tools/build_topology_dump.sh` does not list (nor `src/io/snapshot/interface.c`).
- `narrow_int64_to_int_checked` is defined in `src/core/output_buffer.c:27`, which the script **deliberately excludes** to keep its dependency surface small — but Phase 5's int64 work made `src/io/tree/interface.c:154` call it.

**Suggested fix:** add `src/io/snapshot/registry.c`, and rather than pulling in `output_buffer.c`, consider relocating `narrow_int64_to_int_checked` to `src/util/numeric.c`, where a general numeric-narrowing helper belongs and which the script already builds. That fixes the tool without widening its dependency surface.

### 3.2 Stale comment misdescribing the multiplier contract

`tests/framework/core_test_fixtures.h:213-217` states the shared unit-test config forces `simulation.unique_galaxy_id_multiplier` to `TREE_MUL_FAC` "since the tree-ordered branch these overrides now route through hard-rejects any other value". **Both clauses became false at Slice 7:** the encoder takes the configured multiplier and the tree-ordered rejection was lifted. The forcing behaviour is harmless — it forces the still-valid default — but the comment misdescribes the code. This had no in-plan home: outside Slice 7's surface and outside Slice 11's documentation-only surface.

### 3.3 `GENERATED_HEADERS` listed generated files nothing generates

`$(GEN_DIR)/init_halo_properties.inc` is listed as a dependency, but no script under `scripts/` emits it any more. The file exists on disk as a leftover artifact. Remove the stale dependency (and the orphaned artifact).

**Fixed 2026-08-13, and widened to its sibling.** Both `init_halo_properties.inc` and `init_galaxy_properties.inc` were verified orphaned on both ends — nothing under `scripts/` emits them and no C source includes them — so both entries were dropped from `GENERATED_HEADERS` and the leftovers moved to `archive/orphaned-generated/`. Both had been untracked since `ba8f8e60` stopped committing generated files. Removing only the one this section named would have left the three skills that track "the Makefile still names two of them" wrong, so those were updated too (`mimic-properties`, `mimic-debugging-playbook`, `mimic-architecture-contract` W4). `reset_galaxy_properties.inc` and `tests/generated/module_sources.mk` remain on disk in the same legacy class but were never named by the Makefile and are left alone.

**Updated 2026-08-14.** `reset_galaxy_properties.inc` was verified orphaned on both ends by the same test (nothing under `scripts/` emits it, no C source includes it, the Makefile never named it) and moved to `archive/orphaned-generated/` alongside its two siblings, with `check-generated` re-run. The three skills that track this class were updated with it. `tests/generated/module_sources.mk` is still on disk and still left alone.

### 3.4 `scripts/discovery.py` has no `micro-uchuu-snapshot` entry

`FULL_MODEL_TEST_SIMULATIONS` and `PRODUCTION_TEST_CONFIG_SIMULATIONS` (`:41-56`) list `micro-uchuu`, `micro-uchuu-hdf5` and `micro-uchuu-ascii` but not `micro-uchuu-snapshot`. Carried forward from the Phase 4b reader plan's deferred list. Nothing in the identity gate consumes these lists, so this is a gating-completeness question, not a correctness one.

### 3.5 Gate hardening: an unreachable gap and a runner annoyance — CLOSED 2026-08-20

- `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py` — stage 8's `assert_records_byte_identical()` returns a record count but never asserts it is greater than zero. **Not reachable today:** stage 8 opens with `GATE.require("identity:halos-only:fixed", ...)`, and `compare_pair` hard-fails when `compared_records <= 0` or when it mismatches the independently derived count. A two-line guard whenever this file is next legitimately open.
- `tests/framework/runner.py` does not abort the suite on a failed stage: it records the failure and continues, so every later stage still spins up and fails its prerequisite check. Exit status stays non-zero, so there is no correctness impact — but on a multi-stage gate it wastes time and buries the root cause under redundant failures.

**Both closed 2026-08-20.** The stage-8 guard now lives inside `assert_records_byte_identical()` rather than at the call site, so the function that promises "records byte identical" refuses to return a vacuous zero-record success. `run_test_suite()` gained an **opt-in** `abort_on_failure=False`; the default is behaviour-preserving for every existing caller and only the identity gate passes `True`, since only its entries are ordered stages. Aborted entries emit SKIP naming the stage that stopped the suite, so the one-marker-per-test contract holds and nothing disappears from the record.

### 3.6 Test-coverage gaps recorded during the run — first two CLOSED 2026-08-20

- **No committed self-test for `scripts/compare_cross_format_identity.py`.** Its three synthetic failure modes (perturbed byte, dropped ID, duplicated ID) plus signed-zero, NaN-payload, one-sided-duplicate and corrupt-input cases were all demonstrated as run evidence and independently re-derived by the PM, but nothing committed exercises them. The natural home is beside the gate.
- **No committed regression test for the `UniqueGalaxyIDMultiplier` provenance attribute.** Read-back evidence exists for per-file and master on two packages; a committed test needs `tests/integration/test_output_formats.py`.
- **The Consistent-Trees reader guards lack durable discriminating-multiplier tests.** The three forest-size guards were proven by a temporary reseed experiment and by multiplier-4 rejections on real data, then reverted.
- **The quote-stripping branch in `tests/framework/core_test_fixtures.h` has no test.**

**The first two closed 2026-08-20; the last two remain open.**

`tests/scientific/test_compare_cross_format_identity.py` commits the comparator's failure modes as 15 cases. It is in the **core** tier rather than "beside the gate" as this section originally suggested, and that relocation was deliberate: the gate needs two multi-gigabyte machine-local datasets and takes hours, so nothing registered beside it runs in CI, and a self-test that never runs hardens nothing. The file is package-neutral by construction — it synthesises its own HDF5, imports the comparator from `scripts/`, and never runs Mimic — so the core tier costs nothing and runs it on every default-pair suite. Verified to pass under both the default pair and `halos-only`/`micro-uchuu-snapshot`, which exercises the other processing order.

**The lesson worth carrying, because it cost a review round.** The first version of this file was validated by mutation testing and looked convincing: six deliberate breakages of the comparator, each caught by exactly the right case. An external reviewer then pointed out that all six probed the comparison *predicate* — bytes versus values, tolerant versus exact, which exit status — and none probed *coverage*. Three further mutations were tried and **all three survived**: skipping the last field of the record (`left.dtype.names[:-1]`), comparing only the first shared snapshot, and scanning duplicates on the left run only. Every payload divergence sat in an early field of Snap000, and both duplicate cases put their defect on the left. The fixtures were fixed rather than the count of cases raised: one case now perturbs the *final* field of a *later* snapshot with Snap000 identical, the one-sided duplicate moved to the right run, and the aggregation case now models both real partition layouts with ids reversed within each snapshot (which also catches removal of the per-id sort alignment). All nine mutations are now caught. **Mutation-test a comparator along both axes — what it compares, and what it visits.**

`test_hdf5_unique_galaxy_id_multiplier_provenance` (`tests/integration/test_output_formats.py`) covers the provenance attribute in master and partition. It runs **twice**, once as configured and once at 10^10, for a reason worth recording: every shipped package either declares the default 10^9 or inherits it, so a writer that ignored `MimicConfig.UniqueGalaxyIDMultiplier` and stamped the `TREE_MUL_FAC` constant would record the right number everywhere and pass a default-only test. That producer mutation was applied to `src/io/output/metadata_hdf5.c:221`, rebuilt, and confirmed to fail the strengthened test and pass the original one.

### 3.7 Smaller items, verified and recorded

- **`h_convention` producer/consumer asymmetry.** A future package declaring `units: Mpc/h` with `h_convention: free` would be stamped `100.0`, held as `67.74`, and abort at `open_run` on correctly produced data. Unreachable today; would bite whoever first declares that combination.
- **`snapshot_h5_fill_identity`** derives its memory type from the schema table but hard-codes destination `int64_t` / `ncols=1`. Documented in-code, deliberately unguarded.
- **`expect_success` duplicates `expect_fatal_capture`** and drops its truncation guard.
- **`VIEW_TAKING_OUTPUT_FUNCTIONS`** in `scripts/generate_properties.py` crosses the core/model boundary.
- **`.agents/skills/mimic-properties/SKILL.md:93`** was outside Slice 11's authorized surface; re-check it against the shipped property system.

### 3.8 D8's residual panel findings — ALL CLOSED 2026-08-14 (`2385b480`)

The review panel that supervised D8 raised six findings deliberately left outside both plans' frozen surfaces. Five are closed; the sixth was a history decision, now recorded in `MIMIC-DEVELOPMENT-PATHWAY.md` → "Completed Work". Kept here because two of them corrected beliefs that had already cost time.

- **The HDF5 baseline was never "frozen" at an old record layout, and the note claiming so was false when written.** `scripts/regenerate_baseline.sh` carried a LAYOUT FREEZE note asserting the committed baseline was deliberately held at a pre-precision-widening layout (sidecar `binary_record_size` 224 against the current 264). Measured across the boundary: at `bf0993fa~1` the baseline's `Snap063/Galaxies` dtype was itemsize 224 with `Mvir` float32 and its sidecar agreed; `bf0993fa` regenerated that **data** to 264 / float64 and refreshed the sibling *binary* sidecar but not the HDF5 one. `77ab8462` then read the resulting mismatch as a deliberate freeze, wrote the note, and cited it to justify a surgical byte-edit instead of a re-run. **No cross-layout test coverage ever existed and none was lost:** `test_hdf5_baseline_comparison` decodes through each file's own embedded compound dtype (`load_hdf5_halos`), and `binary_record_size` is consumed only on the binary path. D8's sidecar refresh restored consistency for the first time since `bf0993fa`. The note is retired; see §2.1's dated correction for the belief that produced it.
- **The defect the note misdiagnosed was real.** The script installed only `model_000.hdf5`, while the committed baseline is **seven** tracked files — so any regeneration left the master and `metadata/` describing a different run, which is exactly how the sidecar fell a record layout behind its own data. It now installs the shard, master and whole `metadata/` directory as one set; refuses any `MODEL`/`SIMULATION` other than the pair owning the committed baseline (the comparison test skips for others, so the result would never be checked); writes backups under `archive/baseline-backups/` rather than inside the deliberately un-ignored baseline tree; clears prior output so a stale component cannot be installed as fresh; checks every copy; and exits non-zero instead of reporting success when validation cannot run.
- **The `uchuu` integration tier was red for a test whose assertion was never a contract.** `test_dynamic_timestep_varies_substeps` required a higher-redshift snapshot to report more dynamic substeps than the lowest-redshift one. The `uchuu` fixture is six synthetic halos on snapshots 48–49 only (z=0.0227 and z=0.0000), and every execution reported `num_substeps=3`. Since `N = ceil(time_interval × SubSteps / t_dyn)` is an integer bucket, the trend is not a property of the algorithm. Renamed `test_dynamic_timestep_computes_substeps` and reframed on invariants true for every package, with `MaxDynamicSubsteps` set **below** `SubSteps` so that — because `compute_dynamic_substeps()` always returns within `[1, cap]` — a count equal to `SubSteps` can only mean the fixed scheme ran. `make MODEL=sage16 SIMULATION=uchuu tests-integration` now exits 0 (77 PASS, 0 FAIL). The durable rule is in `.agents/skills/mimic-validation-and-qa`: a core test runs against every package, so it must assert nothing package-specific — literals *or* physical trends.
- **Three smaller items.** `convert_unit_scalar()`'s three reference-side unit lookups reused the caller's `field_name` as the error subject, so a maintainer's misconfigured internal reference would have produced a FATAL blaming the user's input (error-path strings only; unreachable today). `assert_hdf5_schema_layout`'s `expected_format_version` default was removed rather than its self-contradicting docstring reworded — no call site used it, so the signature now enforces what the docstring already demanded. And `reset_galaxy_properties.inc` was verified orphaned on both ends and archived to `archive/orphaned-generated/` beside its 2026-08-13 siblings (see 3.3), with the four trackers that named it corrected.

---

## 4. Carried forward from the Phase 4b reader plan

`docs/dev/MIMIC-SNAPSHOT-READER-PLAN.md` was archived to `archive/dev-plans/` at Phase 5 closeout. Its four still-live deferred entries survive in `MIMIC-DEVELOPMENT-PATHWAY.md` → "Completed Work" and `MIMIC-DUAL-DRIVER-PLAN.md:155`, and are repeated here so one document holds everything:

1. The shared HDF5 read-utilities refactor (downgraded to optional; nothing independently requires it).
2. The empty-dataset-with-non-sentinel-metadata reader check.
3. Reader strictness gaps — nested `/header` objects, same-size datatype acceptance.
4. `scripts/discovery.py` test-gating membership for `micro-uchuu-snapshot` (see 3.4).

---

## 5. Plan defects recorded during execution

Kept for whoever writes the next plan; each cost real time.

| # | Defect | Disposition |
|---|---|---|
| 1 | Slice 7's Tests-allowed clause said "the **two** multiplier precedence cases"; the file held one precedence test plus a rejection test | Resolved on the record. Reword to "the precedence case and the rejection case"; refresh the span. |
| 2 | Slice 8's authorized surface omitted `tests/unit/run_tests.sh` while granting the `tests/unit/` directory, making a criterion unsatisfiable | Resolved on the record. Name the file explicitly, as Slice 4's surface already did. |
| 3 | Slice 10 criterion 1 required `make … tests-scientific` to exit 0, which no in-contract implementation could satisfy — `tests/scientific/test_scientific.py` was hard-bound to `test_binary.yaml`, which a snapshot-ordered package correctly refuses | **Amended** (`cf59271f`, human-approved). The tier now selects HDF5 output when the package cannot write binary. Without this, Shin-Uchuu would have inherited a scientific tier that could not run at all. |
| 4 | Slice 10's "exactly two changed lines" rule forced both new run files to keep their ASCII counterpart's header comment, so each described itself as Consistent-Trees ASCII | **Amended** (`cf59271f`). Now two functional keys plus an optional header comment. |
| 5 | Slice 10 criterion 8's file enumeration omitted `scripts/compare_cross_format_identity.py` while the Authorized Surface two lines below granted it | **Amended** (`cf59271f`). |

**Process lesson for the next Mode B run.** A `start-slice` *relaunch* preserves the original `before_head`; it does not re-record it. Any commit made between slices therefore lands inside the next slice's recorded diff window and fails the floor's surface check. Finalize the in-flight attempt first, then commit, then start the slice fresh.

---

## 6. Shin-Uchuu readiness

**Superseded ordering per `docs/dev/POST-PHASE-5-JOINT-REVIEW.md` §6 (2026-08-13).**

**The Mimic runtime's known production blockers are closed, both previously decided-but-unimplemented runtime slices have since landed, and the converter's scale-engineering pass closed on 2026-08-28 (§6 item 7).** Both drivers are live, the identity gate passes bitwise on two models and two timestep schemes with no tolerance, the tree-ordered path is provably unchanged, and the scientific tier now runs for snapshot-ordered packages. The joint review's two runtime blockers — the galaxy pool's chunk design exhausting the allocator's block table at slab scale, and ignored HDF5 statuses on the output finalization path — were fixed in `74e8a70e` and certified by the gate re-run. What was decided but unbuilt on the runtime side — output partitioning (D5(a)) and the `Spin` units-label reconciliation (D8) — is now closed (§6 items 4 and 5). On the converter side the review found that the implementation — in-memory identity/rank pass, in-memory validation battery, a scatter resume model incompatible with this plan's own batched consumptive-delete transfer — cannot execute the production conversion at all. Those limits had been recorded in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as deferred to a future production pass, with no plan scheduling it (joint review F-13); that pass is the one that closed on 2026-08-28, and the README note now records what each stage does instead.

**The ordered checklist lives in `POST-PHASE-5-JOINT-REVIEW.md` §6 and is not duplicated here** — it was re-sequenced on 2026-08-13 under decision D9 (*freeze the runtime contract → rehearse → scale the converter → run*), and maintaining a second copy is how the two documents drifted apart in the first place. This section carries only the status of each item and the pointer into the detail above.

| §6 item | Status | Detail |
|---|---|---|
| 1. Pool ceiling + HDF5 statuses + D1 consolidation | **CLOSED 2026-08-13** | Landed in `74e8a70e`; gate re-ran green at that HEAD, 8/8 stages, 0 failures — `halos-only` 4,409,282 records / 20 fields, `sage16` 3,111,793 (fixed) and 3,111,759 (dynamic) / 42 fields, all bitwise identical under both timestep schemes, plus stage 8 confirming 4,409,282 tree-path records byte-identical to `ae22d278`. Every figure matches the pre-change record exactly |
| 2. `Spin` bounds (D6/D7) | **CLOSED 2026-08-26 from measurement.** `shin-uchuu`'s `[-1000, 1000]` is **not refuted** by the rehearsal — a scan of all 406,668,896 halos measures `[-11.673591, +17.797567]`, zero non-finite — and `Vmax`'s inherited `[10.0, 5000.0]` floor was found wrong for both shin-uchuu packages and corrected to `[0.0, 5000.0]` (measured minimum 1.77 km/s at `Len` = 2). `uchuu` keeps `[-5000, 5000]` and still awaits its own dataset | §2.1; JR §6 item 2 |
| 3. Memory recompute + 10⁹ population check | **CLOSED 2026-08-26 from measurement.** Population check clear: measured `P`/N = 0.99701 gives P ≈ 3.54 × 10⁸, **2.83× clear** of `MAX_HALO_ARRAY_SIZE` = 10⁹ and 6.07× of `INT_MAX`. Memory closed on **measured peak RSS**, not the parametric form: `RSS = 1.221 GB + 960.9 B/halo × N + 2 × 176 B × pool_allocated(N)`, reproducing 2.184 GB at N = 621,360 and 14.301 GB at N = 9,006,294, projects **≈476.6 GB** at the largest slab (N ≈ 3.546 × 10⁸) after item 11's 5% seed headroom. The trigger is crossed and the compact previous-slab projection is **deferred by owner decision**, not required | §2.2, §2.3; JR §6 items 3 and 11 |
| 4. Output partitioning (D5(a)) | **CLOSED 2026-08-13** — landed `3e31cc0c`/`7b68e01d`, certified by a cross-format identity gate re-run (8/8 stages) on the real micro-Uchuu dataset; documentation of record closed in `ce689907`…`e9619440`, integration-tier filenames in `1cb3208e`. See `SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` | One file per requested output snapshot, no size knob; its own frozen slice with its own gate re-run |
| 5. `Spin` units-label reconciliation (D8) | **CLOSED 2026-08-14** — all eight packages now declare `units: Mpc/h km/s` with an accurate specific-angular-momentum description; `micro-uchuu`'s range widened to `[-1000, 1000]`; the bitwise tree-path proof confirmed every `Spin` value byte-identical before and after. See `D8-SPIN-UNITS-RECONCILIATION-PLAN.md` | §2.1. Affects **all eight** packages (every one stores J/Mvir; the "two different quantities" claim was wrong), needed a `scripts/generate_properties.py` `UNIT_REGISTRY` entry and a reference dimension, and carried an h-convention path that could silently move `Spin` values — so the binding criterion was byte-identical values with only the label changed |
| 6. Subset conversion + complete rehearsal | **CLOSED 2026-08-26.** Unblocked 2026-08-25 (operator at the machine, source access confirmed) and then run: a whole-forest subset was selected and extracted, converted, and exercised end to end at **406,668,896 halos**, `sage16` and `halos-only` both, which is what closed items 2, 3 and 9 from measurement | §2.2 and §2.5 carry the measurements it produced; JR §6 item 6 and the item-6 unblocking note are the record of the rehearsal itself. The subset satisfies the composition constraint **except** for the percolation super-forest, which the tree driver cannot load — see the carry-forward below |
| 7. Converter scale-engineering pass (D4) | **CLOSED 2026-08-28** — eight code slices (`184424df`…`b2ae9601`, plus `0ab453fe` early; plan archived on completion), gated on a **fresh** micro-Uchuu conversion the rebuilt converter itself produced: battery 15/15, cross-check 8/8, zero unexplained mismatches. Rehearsal-scale memory, warm and repeated: `links` 76.39 → 9.76–10.01 GB, battery 73.27 → 3.25 GB, cross-check 251.32 → 15.84–16.56 GB warm; production storage 6.89 TB ≤ 7.0 TB | `SHIN-UCHUU-CONVERSION-PLAN.md`'s "Pre-conversion obligation" → "Pass complete", and `POST-PHASE-5-JOINT-REVIEW.md` §6 item 7. **Carried forward:** the per-snapshot window is now the binding memory term (≈225–235 GB projected at the production largest slab, to be re-derived from the production conversion report) |
| 8. Particle mass | **CLOSED 2026-08-14** — 8.97 × 10⁵ Msun/h, confirmed from Ishiyama et al. 2021 ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)); corrected a recorded value that was low by 10× | §2.4 |
| 9. Remaining property ranges | **CLOSED 2026-08-26 from measurement.** `Len`'s core contract `[1, 10⁹]` with `sentinels: [0]` is confirmed unchanged (catalog range [2, 13,575,251], `len_zero` 0 on all 70 snapshots). `deltaMvir`'s core range `[-20000, 20000]` is **not widened**, deliberately: measured `[-58.55, +78.72]` for `sage16`, but the micro-Uchuu anchor puts a plausible production maximum at 1.5–2 × 10⁴, i.e. at or near the bound. `Spin` and `Vmax` belong to item 2 | §2.5; JR §6 item 9 |
| 10. Uchuu-family particle mass 0.6% low | **Open — NEW 2026-08-14** | §2.7. Six packages declare 3.25e8 where the consistent value is 3.27e8. Not a config-only fix: the header-agreement check aborts every snapshot run until the fixture and the real dataset are re-stamped, and `virial.c:51` moves every subhalo's mass. Not a Shin-Uchuu blocker |

**Carried into the production run — closed as checklist items, still owed as work.** Closing an item from measurement does not discharge the obligations that measurement itself created, so those are kept here rather than folded into the rows above.

- **From item 2 — the `Spin` bound is set by a scan of the production dataset, and that is a binding pre-run gate.** The rehearsal subset's ~56× margin is an artefact of the subset, not a property of the box: it excludes the percolation super-forest, so its most massive halo is 1.2177 × 10¹³ Msun/h against the box's sampled ≥1.2288 × 10¹⁵. A mass-matched anchor (micro-Uchuu's 270.30 at 3.09 × 10¹⁴ Msun/h, scaled by Mvir^(2/3)) puts the production expectation near **680** against D7's 1000 — ~1.5× margin, not 56×. `report.py` does not record value extrema, so this is a separate `h5py` scan.
- **From item 3 — re-project the memory peak from the production conversion report's own per-snapshot slab counts before committing to the run, using `C`/N = 1.05.** That report gives the real largest slab and removes the one extrapolation the 476.6 GB projection could not avoid. Run the job on a lean machine. The compact previous-slab projection stays specified and available if the re-projection comes in materially higher. **Done 2026-09-04** (`SHIN-UCHUU-CONVERSION-PLAN.md` → "P3b, P4 done and measured") — the re-projection came in materially higher (largest slab 519,342,987 halos, ≈639–697 GB against ≈540 GB available): **STOP**, not proceed. The production run is relocating to an NT large-memory node rather than reinstating the compact projection, since that alone does not close the gap.
- **From item 6 — the rehearsal could not exercise the excluded super-forest**, which holds 56.0% of all halos, so no rehearsal figure bounds the box's maximum from above. This is the reason item 2's production scan is binding rather than a formality.
- **From item 9 — `deltaMvir` is a post-run validation, not a pre-run scan.** It is created during inheritance (`src/core/inheritance.c`) and exists only in Mimic output, never in the input catalog, so it can only be checked against the production run's own output.
- **From item 7** — the per-snapshot window carry-forward is stated in the row itself.

Nothing else in §3 blocks the conversion; §3.1 does, and was reclassified accordingly (D10).
