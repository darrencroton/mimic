# Mimic HOD and SHAM Implementation Plan

**Purpose:** Add one generic core mechanism for creating galaxy records during processing, build a new halo occupation distribution (HOD) model package as its first application, and replace the `sham` model package wholesale with a calibrated global rank abundance-matching model on the snapshot-global mode.

**Status:** Planning contract, revision 3 (2026-10-04). Nothing below is implemented. The owner froze every scientific and architectural decision in this file on 2026-10-04 after an investigation of the code at the planning baseline and an independent read-only design review (Codex `gpt-6-sol`, high effort) of the record-creation design; the decisions are recorded under [Frozen Decisions](#frozen-decisions). Revision 1 was reviewed by an independent panel (Codex `gpt-6-astra` and Claude Fable 5.1, both high effort, read-only); every finding was verified against the repository and revision 2 resolves them: the identity budget no longer fails valid runs that create nothing, the galaxy pool and the vertical workspace allocator are inside the slices that need them, satellite offsets are converted from physical to comoving coordinates, the fixture's creation parameter is optional, the HOD and SHAM receipts carry their binding equations, the statistical gates are stated for the distribution they test, the SHAM candidate forecast was replaced by measured counts, and the stale anchors were corrected. Revision 3 followed the panel's focused re-review: the SHAM retirement test case now names the fixture rows that are actually demoted (snapshot-4 halos 0 and 2), the HOD module no longer declares a property it does not read, and the snapshot callback's entry validation states what a Type 2 entry would mean. The science context is [`MIMIC-HOD-SHAM-REVIEW.md`](MIMIC-HOD-SHAM-REVIEW.md); where this plan and the review differ, this plan governs, because the review predates the owner's decisions.

**Planning baseline:** the last commit that changes anything outside this file, [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) and the status note of the review. Every `path:line` anchor below was read at that baseline. The hash is deliberately not written here: PM binds a run to the SHA-256 of this file's bytes, so recording a hash that moves with unrelated commits would force a re-freeze. Recheck drift against the anchors before executing a slice; do not silently rebase a contract onto a changed interface.

**Owner:** [Development pathway, next focus](MIMIC-DEVELOPMENT-PATHWAY.md#next-focus-hod-and-sham-models). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md).

## Frozen Decisions

| # | Decision | Owner's choice (2026-10-04) |
|---|---|---|
| D1 | Record-creation mechanism | **Append at the callback boundary.** A full-halo module in any FoF phase requests records through one core function; core stages them during the callback and appends them to the primary FoF workspace when the callback returns, so every later module, phase, substep and by-galaxy pass in the same snapshot sees them as ordinary array entries. Created rows are marshalled inside their host's output segment and inherited like any Type 2 row. The snapshot scope stays topology-immutable. |
| D2 | Who may create | Full-halo callbacks only (`process_full_halo`), in any FoF phase, mirroring the event-emission rule. By-galaxy, per-event and snapshot callbacks cannot create. |
| D3 | Events and created rows | An event may not target a row created in the same callback; `module_emit_event()` rejects the index and the module fails. Later callbacks may target created rows normally. |
| D4 | Both drivers | The mechanism works under the vertical and horizontal drivers through the shared evolution and marshalling path. |
| D5 | Identity | **Scalar negative-ID namespace.** `UniqueGalaxyID = -(1 + ordinal + K * host_key)` with `K = 1024` created records per host (a fixed identity radix; exceeding it fails, never clips). The host key is the host halo's catalogue position: horizontal `row + rows_per_unit * snapshot`, vertical `halonr + rows_per_unit * forest_global`. A new vertical reader hook supplies the run-wide largest forest size, and may answer "unknown", in which case the multiplier `M` is the radix. Each driver computes at startup whether the key space fits int64 and logs the result; a run that creates no record is never affected, and the first `module_create_record()` call in a run whose space does not fit fails with both numbers. Pair identity (source-halo ID plus an ordinal column) is the recorded escape if a future dataset exceeds this. Measured budgets: horizontal full Uchuu about 2^49; vertical and horizontal full Millennium 7.5e15 and 1.2e12 against 9.2e18; vertical Shin-Uchuu ASCII and vertical full Uchuu do not fit (their runs are unaffected; they cannot create records). |
| D6 | HOD package | New `models/hod/` package; one dual-mode module `hod_populate` (`process_full_halo` in `post_timestep`, `process_snapshot` in `post_snapshot`). Externally calibrated parameters; the snapshot callback is a whole-box audit, not an in-run `M_min` solve. |
| D7 | HOD sample | Zheng, Coil & Zehavi (2007) SDSS `Mr < -20` five-parameter fit as the shipped default: `log Mmin 12.02, sigma_logM 0.26, log M0 11.38, log M1' 13.31, alpha 1.06` (masses in `Msun/h`; the fit's halo mass is 200x mean density, its cosmology `Omega_m 0.3, sigma_8 0.9, h 0.7`). All five are run-file parameters. |
| D8 | HOD placement | Satellites at NFW radii inside `Rvir` with Duffy et al. (2008) `c = 5.71 (M / 2e12 Msun/h)^-0.084 (1+z)^-0.47` (Table 1, NFW, full sample, 200c, z 0-2), isotropic directions, the physical offset converted to comoving coordinates, periodic wrap; velocity = host bulk velocity plus three independent Gaussians with `sigma_1D = Vvir / sqrt(2)`. All five concentration numbers are run-file parameters. |
| D9 | HOD lifetime | Synthetic satellites are regenerated on every output snapshot; inherited copies and tree-born Type 2 rows are retired to Type 3 by the HOD module before marshalling. A model-owned flag `HODGhost` marks scaffold rows that are not sample members. |
| D10 | SHAM package | `models/sham/` is replaced wholesale: both legacy modules, the vertical run files, the stale properties and `parameter_units.yaml` go; one dual-mode module `sham_rank_match` (`process_full_halo` in `pre_timestep`, `process_snapshot` in `post_snapshot`) replaces them. |
| D11 | SHAM target | Baldry et al. (2012) GAMA z < 0.06 double Schechter (`log10 Ms 10.66, phi1 3.96e-3, alpha1 -0.35, phi2 0.79e-3, alpha2 -1.47`, physical `Msun` and `Mpc^-3` at `h = 0.7`, Chabrier IMF, stated validity `10^8` to `10^11.5 Msun`), rescaled to the simulation's `h` as `M* ~ h^-2` and `n ~ h^3`. Candidates below the target's low-mass validity floor are masked, never extrapolated. Assignment only on output snapshots inside a declared redshift window; startup fails if an output snapshot lies outside it. |
| D12 | SHAM scatter | None in this plan. Intrinsic scatter with deconvolution is the recorded next extension. |
| D13 | SHAM candidates | Resolved-only: Type 0 and Type 1 rows with accumulated `ShamVpeak` at or above a run-file completeness floor; carried Type 2 rows are retired to Type 3 in `pre_timestep`. The orphan-inclusive mode is a recorded follow-up. |
| D14 | Clustering figure | One model-neutral real-space `xi(r)` helper (periodic cell-grid pair counts, analytic random term) in `plot/mimic-plot/output_utils.py`, used by a HOD figure and a SHAM figure. Projected `wp(rp)` with an SDSS overlay is a recorded follow-up. |
| D15 | CI | `make tests-snapshot-global` (which gains `hod` and new `sham` groups) is added to the CI `horizontal-v3` job. |

## Outcome and Limits

After this plan, a module can create galaxy records that its own snapshot processes and writes, under either driver, through one documented core operation with deterministic identity. `models/hod/` populates FoF hosts with a threshold-sample HOD and writes a self-auditing catalogue. `models/sham/` rank-matches resolved halos and subhalos to a named observed stellar mass function. Both packages ship run files for the real 100 Mpc/h micro-Uchuu horizontal box and for the committed fixture, carry their own tests in the snapshot-global battery, and plot the figures that recreate their source papers' headline results: the mean occupation function with the analytic law overlaid, the satellite phase-space check, the real-space correlation function, and the stellar mass function against its target.

This plan does not implement: an in-run HOD `M_min` solve, a conditional stellar mass function, assembly bias or any secondary-property HOD, SHAM scatter, orphan-inclusive SHAM, projected clustering or any observational clustering overlay, creation from the snapshot scope, event targeting of rows created in the same callback, distributed or chunked processing, production-scale memory measurement, or any observational parity claim. The HOD parameters were fitted to SDSS with a different halo mass definition and cosmology than Mimic's catalogues carry; the shipped runs are framework demonstrations with published parameters and say so. The real-data runs are single process and fully resident, as the horizontal driver is today.

## Repository Evidence and Design Decisions

Anchors read at the planning baseline and re-verified by the revision 1 panel. The binding obligations are repeated inside the slice receipts so PM's Developer and Reviewer prompts carry them; this section explains why the contracts look the way they do.

- A record is a `struct Halo` row (halo-side fields including `Pos`, `Vel`, `Type`, `HaloNr`, `CentralHalo`, both unique IDs, `dT`, `Mvir`, `Rvir`, `Vvir`, `CentralMvir`) plus `struct Halo.galaxy`, a pointer to a pool-owned `struct GalaxyData` (`src/include/generated/property_defs.h:21-48`). Pool pointers never move (`src/core/galaxy_pool.h:26-33`); `galaxy_pool_alloc()` takes an explicit pool (`:73-76`). The vertical driver's pool is the global `VerticalGalaxyPool` (`src/core/allvars.c:37`, `build_model.c:446`); the horizontal driver's is per generation (`horizontal_driver.c:198`).
- The FoF workspace is one contiguous `struct Halo` array; each source subhalo's rows are one contiguous slice recorded as a segment (`src/core/output_buffer.h:24-31`; filled at `src/core/build_model.c:167-173` and `src/core/horizontal_driver.c:648-655`). It is grown only before inheritance (`ensure_fof_workspace_capacity`, `build_model.c:279-307`; `horizontal_ensure_workspace_capacity`, `horizontal_driver.c:257-283`), by two near-duplicate functions over two different owners: the vertical globals `FoFWorkspace`/`MaxFoFWorkspace` (`allvars.c:29, 50`), allocated and freed in `src/io/vertical/interface.c:155-172, 189-201`, and the horizontal `state->workspace`/`workspace_capacity`.
- `marshal_workspace_to_output_buffer()` (`src/core/output_buffer.c:22-71`) copies each segment's slice into the output buffer, skipping Type 3, and writes back each source halo's contiguous output range (`build_model.c:188-191`, `horizontal_driver.c:663-666`); next-snapshot progenitor gathering reads those ranges (`build_model.c:265-275, 371-388`; `horizontal_driver.c:470-520`). Each source halo's rows must therefore stay contiguous in the output.
- `execute_module_pipeline()` (`src/core/module_registry.c:1158-1176`) hands the same `halos` pointer and `ngal` to every phase; `execute_phase()` (`:1017-1087`) caches them for event dispatch (`:62-75, 781-798`), runs full-halo modules then by-galaxy modules galaxy-major, and `ctx->central_galaxy` is set once per FoF (`src/core/halo_evolution.c:95`). The `process()` signature is a frozen ABI (pathway standing constraints). `struct ModuleContext` carries no processing-mode field (`src/core/module_interface.h:176-290`). Growing the array between callbacks, never during one, and refreshing exactly these caches is what makes D1 safe.
- Event emission is allowed only from full-halo callbacks (`emission_allowed`, `module_registry.c:1041-1046`) and target indices are validated against the cached count (`:970-981`). D2 and D3 mirror this.
- The only birth site today is `init_new_halo()` (`src/core/inheritance.c:97-107`); the static `make_orphan()` (`:79-95`) zeroes `Mvir` and `Len`, keeps `Rvir`/`Vvir`, records the host's `Mvir`, `Vvir`, `Vmax` as the infall values when the row was Type 0, and sets Type 2; `set_local_centrals()` (`:109-146`) FATALs unless exactly one Type 0/1 per non-empty slice at inheritance. Created rows are Type 2 orphans of a Type 0/1 host so this invariant holds when they are inherited.
- `Rvir` is recomputed from the critical density at the row's redshift (`src/core/virial.c:102-119`) and is therefore a physical length, while `Pos` is comoving (`simulations/micro-uchuu-horizontal/halo_properties.yaml:85`); a physical offset must be multiplied by `(1 + z)` before it is added to a comoving position.
- Identity: `UniqueGalaxyID = halonr + M*(forest+1)` (`src/include/galaxy_id.h:32-51`); horizontal open-time bounds, which accept a dataset with no halos (`src/io/horizontal/interface.c:102-130`; `struct HorizontalRunInfo` with `snapshot_count` at `src/io/horizontal/reader.h:47-58`); horizontal `HaloNr` is the slab row (`horizontal_driver.c:526-552`); the vertical driver scans every partition's unit count on every rank (`src/core/vertical_driver.c:137-139, 179, 372`) through `count_partition_units` (`src/io/vertical/reader.h`; implementations `binary.c:46`, `hdf5.c:245`, `read_ctrees_hdf5.c:1789`, `read_ctrees_ascii.c:603`). Shin-Uchuu's tree IDs already use about 61 bits (`M = 2e10`, 1.67e8 forests; `simulations/shin-uchuu-ascii/simulation_info.yaml:16, 30`), which is why created identity needs a compact per-driver host key rather than the host's ID, and why a budget check must never abort a run that creates nothing.
- Measured on 2026-10-04 from the 512 local Millennium L-Halo headers: 14,329,882 forests, 760,667,000 halos, largest forest 514,194 halos; horizontal Millennium has 64 snapshots and a largest slab of 18,619,466 rows.
- Measured on 2026-10-04 on the real micro-Uchuu horizontal box at snapshot 49: 561,266 rows, of which 44,978 have `Vmax >= 80 km/s`; an all-ancestor propagation of maximum `Vmax` bounds the SHAM candidates above 80 km/s by 75,117, a physical density of at most `0.0233 Mpc^-3`, below the target's floor density `n(>10^8) = 0.0303 Mpc^-3`.
- `tests/integration/test_unique_galaxy_id_encoding.py:92` asserts every ID is positive on the generic empty pipeline; created records appear only when a module creates them, so that test is unaffected, but its docstring must describe the negative namespace.
- The framework fixture's parameters are set explicitly by tests outside any model package (`tests/integration/test_module_pipeline.py:71-247`, `test_phase_execution.py:50-184`, `test_galaxy_major_loop.py:55, 123`, `tests/unit/test_module_configuration.c:33`, `tests/unit/test_snapshot_module_contract.c:84`), and a missing fixture parameter fails `init()` (`src/module_system/test_fixture/test_fixture.c:72-79`); a new fixture parameter must therefore be optional.
- Horizontal runs write HDF5 only, are single process and run `post_snapshot` after marshal (`horizontal_run_post_snapshot()`, `horizontal_driver.c:1263-1275`, called at `:1639`); `process_snapshot`'s contract forbids topology changes (`src/core/module_interface.h:420-451`).
- `tests/unit/run_tests.sh:39-49` falls back to the project default selectors when `MODEL`/`SIMULATION` are not in the environment, so every standalone test command for a non-default package must export them.
- Model packages are discovered by directory; the only mandatory file is `model_properties.yaml` (`scripts/discovery.py:175-191`, `scripts/generate_test_inputs.py:114-117`). Module tests register in the default tiers only for the vertical `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py:46-53`, `scripts/generate_test_registry.py:163-167`), so snapshot-mode tests run by path from `tests/manual/run_snapshot_global_battery.py` (groups `:80-117`, marker policy `:142-157`: any FAIL/ERROR/SKIP fails, PASS+WARN must equal the declared `TEST_RUN(`/`def test_` count).
- There is no `bool` property type; a flag is `type: int` with `init_value: 0` and `range: [0, 1]` (`scripts/generate_properties.py:57-100, 813`).
- The plotting registry is model-local (`models/<m>/plots/figures/__init__.py`), observational overlays are inline arrays, there is no scipy and no pair-counting code anywhere (`requirements.txt`; grep for `cKDTree`, `xi(`, `pair count` is empty). `plot/mimic-plot/tests/test_plotting.sh` is standalone and not part of `make tests`.
- CI (`.github/workflows/ci.yml:95-127`) runs the default pair and a `horizontal-v3` job; no non-default model other than `halos-only` is exercised.
- The existing `models/sham` is inventoried in the review; the external couplings a wholesale replacement must update are `Makefile:883-890` and `:505-506, 517`, `tests/manual/run_snapshot_global_battery.py:47, 104-116`, `scripts/fuzz_pipeline.py:130-134, 200`, `scripts/generate_properties.py:626`, `tests/README.md:29-30, 62, 174`, `docs/USER-GUIDE.md:59, 303-306, 398, 707, 717`, `docs/DEVELOPER-GUIDE.md:5`, `plot/mimic-plot/README.md:83, 162`, `AGENTS.md:71`, `CHANGELOG.md`, `models/sham/plots/figures/stellar_mass_function.py:64-101` (reads properties the new model removes), and the skills named in Slices 8 and 10. The review's own link to the legacy module README was converted to plain text when this plan was committed, so `make check-docs` stays green when the module is removed.

## The Record-Creation Contract

This is the contract Slices 1 to 3 implement and Slice 10 publishes. It is stated once here and bound into those slices' acceptance criteria.

**API.** `int module_create_record(struct ModuleContext *ctx, int host_index, struct Halo **row)` in `src/core/module_interface.h`, implemented in `src/core/module_registry.c`. Legal only while a `process_full_halo` callback is running (the same gate as event emission); any other caller gets an error return and the module fails. `host_index` must name a committed workspace row of Type 0 or 1 with a non-null galaxy; created rows cannot host. On success it returns the new row's future logical index (`>= committed count`) and sets `*row` to a staged row the caller may fill until its callback returns.

**Initialisation.** The staged row is a copy of the host's halo-side fields passed through `make_orphan()`, which Slice 3 exports from `inheritance.c`: Type 2, `Mvir` and `Len` zero, `deltaMvir = -host Mvir`, `Rvir` and `Vvir` kept, and, because the host is Type 0 or 1, the infall fields become the host's current `Mvir`, `Vvir`, `Vmax` when the host is Type 0 and stay the host's recorded values when it is Type 1. `HaloNr`, `CentralHalo`, `CentralMvir`, `UniqueCentralGalaxyID`, `SnapNum`, `dT`, `Pos` and `Vel` start as the host's. `UniqueGalaxyID` is the created ID below. `galaxy` is a fresh slot from the workspace's pool initialised by `init_galaxy_defaults()`. The module then sets whatever physics it owns.

**Commit.** When the callback returns, core appends every staged row, in creation order, to the end of the primary workspace, growing it through the shared workspace growth function, and refreshes the dispatcher's array pointer and count, `ctx->central_galaxy` (same index, new address), and the event-dispatch state. The next module receives the complete contiguous array. The by-galaxy pass visits created rows as ordinary entries. Later phases and substeps see them. No cached pointer is ever used across a growth.

**Marshal.** `struct FoFWorkspace` carries `base_count`, the row count when the pipeline started, and a capacity-tracked `created_host` array giving, for each row at or beyond `base_count`, the workspace index of its host. `marshal_workspace_to_output_buffer()` emits, per segment in segment order, the slice's surviving rows followed by that host's surviving created rows in creation order (a created row belongs to the segment whose slice contains its host), and counts both in `output_count`, so `FirstHalo`/`NHalos` and next-snapshot gathering are unchanged. Created rows are inherited as Type 2 like any orphan; their lifetime from then on is the model's.

**Identity.** `UniqueGalaxyID = -(1 + ordinal + MAX_CREATED_RECORDS_PER_HOST * host_key)`, `MAX_CREATED_RECORDS_PER_HOST = 1024` in `src/include/constants.h`, `ordinal` counted per host within the FoF step in creation order. `host_key = row + rows_per_unit * unit` where the driver publishes `(unit, rows_per_unit)` once per processing unit through `struct RecordIdentitySpace`: horizontal `unit = snapshot number`, `rows_per_unit = max over [0, snapshot_count) of snapshot_halo_count()`, `row = host HaloNr` (the slab row); vertical `unit = GlobalForestOffset + unit index`, `rows_per_unit = run-wide largest forest size from the new reader hook, or M when the hook answers unknown`, `row = host HaloNr` (the in-forest index). The largest magnitude the encoding can produce is `1024 * units * rows_per_unit`, so the space fits iff `1024 * units * rows_per_unit <= INT64_MAX`, evaluated without intermediate overflow, with `units` the run's unit count (`HorizontalRunInfo.snapshot_count` horizontal, total forests vertical) and a space with zero units or zero rows counted as fitting (nothing can be created in it). Each driver evaluates this once at startup, logs the three numbers and the verdict at INFO, and records the verdict; a run that never creates a record is never affected. The first `module_create_record()` call in a run whose space does not fit returns the error with both numbers, the radix and the driver name, so the module fails and the run stops there. Created IDs are deterministic for a fixed dataset and run file, independent of traversal order and partitioning, unique run-wide, and negative; tree rows keep their positive IDs unchanged. Created IDs are not identical across drivers; the cross-format identity gates compare tree rows only and no model in this plan runs under both drivers for comparison.

**Memory.** Created `GalaxyData` comes from the workspace's pool (counted in `G`), rows end in the output buffer (counted in `C`/`P`), and the staging and per-host ordinal scratch are run-persistent grow-to-high-water allocations in `MEM_HALOS` released at driver teardown. The horizontal generation footprint check still covers only the seeded allocation; in-sweep growth from created rows is reported, as today.

**Footguns stated to module authors.** A module creating in a substep phase creates once per substep unless it guards on `ctx->substep_number`. The staged row pointer is callback-scoped. Retiring a slice's only Type 0/1 while leaving created dependants makes the next snapshot's inheritance FATAL, as it does for tree orphans today. A module cannot tell its dispatch mode from `ModuleContext`; a module that creates must be configured as `process_full_halo`, and a by-galaxy configuration of it fails at its first creation call.

## HOD Model Specification

Frozen science for Slices 4 to 6, repeated in the receipts that bind it. Units: `Mvir` is `1e10 Msun/h` internally; occupation masses are `Msun/h`, so `M = Mvir * 1e10`.

- **Hosts.** Each FoF group's Type 0 row, read from `ctx->central_index`. Type 1 rows are scaffold only.
- **Occupation (Zheng et al. 2005; Zheng, Coil & Zehavi 2007 equations 2 and 5).** `<Ncen> = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]`. Central present if `u0 < <Ncen>`. If present and `M > 10^HODLogM0`: `lambda = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha`, `Nsat ~ Poisson(lambda)` by inversion from one uniform with sequential cumulative search; otherwise `Nsat = 0`. Central gating is the convention; satellites are never drawn without a central. `lambda >= 1024` or a drawn `Nsat > 1024` is an error (the identity radix), never a clip.
- **Spot values (hand-checkable, Mr < -20 parameters):** `<Ncen>(10^12.02) = 0.5` exactly; `<Ncen>(1e14) = 1` to 1e-12; `lambda(1e13) = 0.457322`, `lambda(1e14) = 5.373959`, `lambda(1e15) = 61.842859` to six decimals.
- **Concentration (Duffy et al. 2008).** `c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC` with defaults `5.71, 12.301030, -0.084, -0.47`; `c(2e12, z=0) = 5.71`, `c(1e14, 0) = 4.110765`, `c(1e12, 1) = 4.369568`, each to 1e-6 (the rounded pivot gives `5.7100000048` at exactly `2e12`).
- **Radius.** `r_phys = Rvir * x / c` where `x` solves `m(x) / m(c) = u`, `m(x) = ln(1+x) - x/(1+x)`, by bisection on `[0, c]` to `|m(x)/m(c) - u| <= 1e-12`; `Rvir` is the host row's core field, a physical length. Spot values: `u=0.5, c=5: x = 2.2166041757`; `u=0.1, c=10: x = 0.8223966449`; `u=0.9, c=5.71: x = 4.9207968140`.
- **Direction, comoving conversion and wrap.** `cos theta = 2u - 1`, `phi = 2 pi u`; the comoving offset is `r_com = r_phys * (1 + z)` with `z = ctx->redshift`; `Pos = host Pos + r_com n`, each component wrapped into `[0, BoxSize)` so that exactly `BoxSize` maps to `0`.
- **Velocity.** `Vel = host Vel + (g1, g2, g3) * Vvir / sqrt(2)`, independent standard Gaussians; `Vvir` is the host row's core field (`Vvir = 200` gives `sigma_1D = 141.421356`).
- **Random numbers.** A model-private counter-based generator (splitmix64 over a key, Box-Muller for Gaussians, after the pattern of the legacy `sham_assign_stellar_mass.c:54-77`), keyed by `(HODSeed, snapshot_number, host UniqueGalaxyID, draw index)`. The exact salt schedule is implementation-defined; the contract is bitwise repeatability for identical input and parameters, invariance under FoF and row permutation, and independence of traversal order and partitioning.
- **Lifecycle per FoF step (`post_timestep`, every processed snapshot).** (1) Every inherited Type 2 row is retired to Type 3. (2) `HODGhost = 1` on every row. (3) If the snapshot is in `output.snapshot_list` (an empty list means every snapshot), draw the host: a present central sets the Type 0 row's `HODGhost = 0`; each satellite is created through `module_create_record()`, placed as above, with `HODGhost = 0`.
- **Audit (`post_snapshot`, output snapshots only).** Over the population: `n_expected = sum over Type 0 rows of (<Ncen> + <Ncen> lambda) / V`, `n_realised = count(HODGhost == 0) / V`, the same two for satellites alone, and `<N(M)>` realised versus expected in 0.2 dex bins over the host mass range (volume `V = BoxSize^3` in `(Mpc/h)^3`). One summary line per output snapshot at INFO in a fixed, documented format (`HOD audit z=<z> hosts=<n> n_gal expected=<x> realised=<y> f_sat expected=<a> realised=<b>`); the per-bin lines at VERBOSE. Non-output snapshots return without output.
- **Properties.** `HODGhost` (int, dimensionless, `init_value 0`, `range [0, 1]`, output, description stating 1 = scaffold row outside the sample, 0 = sample member). No stellar mass: a threshold HOD assigns none.
- **Parameters** (all required in `modules.parameters`, validated finite and in range at init): `HODLogMmin`, `HODSigmaLogM (> 0)`, `HODLogM0`, `HODLogM1`, `HODAlpha (>= 0)`, `HODSeed (int >= 0)`, `HODConcA (> 0)`, `HODConcLogMpivot`, `HODConcB`, `HODConcC`.
- **Caveats the README states.** The fit's halo mass is 200x mean density and its cosmology differs from the catalogues; Mimic's `Mvir` is the catalogue virial mass (Millennium M200c, Uchuu Rockstar `Mvir`); the output therefore demonstrates the framework, not a calibrated SDSS mock. The ghost scaffold stays in output; consumers filter on `HODGhost == 0`.

## SHAM Model Specification

Frozen science for Slices 7 to 9, repeated in the receipts that bind it.

- **Candidates.** Type 0 and Type 1 rows with `ShamVpeak >= ShamMinVpeak`. Type 2 rows are retired to Type 3 in `pre_timestep` before anything else runs.
- **Peaks (`pre_timestep`, every processed snapshot).** For Type 0/1 rows: `ShamVpeak = max(ShamVpeak, Vmax)`, `ShamMpeak = max(ShamMpeak, Mvir)` with the existing float-bound rule (the double `max` must be `<= FLT_MAX` before the cast; `Mvir = FLT_MAX` stored exactly, `1e39` fails). `ShamGhost = 1` and `StellarMass = 0` on every row.
- **Target.** `Phi(M) dM = exp(-M/Ms) [phi1 (M/Ms)^alpha1 + phi2 (M/Ms)^alpha2] dM/Ms`, `n(>M) = integral_M^inf Phi dM'`. Published values at `h_obs = ShamTargetHubble` are converted once at init to the simulation's `h`: `log10 Ms_sim = ShamTargetLogMstar + 2 log10(h_obs / h_sim)`, `phi_i_sim = phi_i (h_sim / h_obs)^3`. Worked example (Baldry+12, `h_sim = 0.6774`): `log10 Ms_sim = 10.68851`, `phi1_sim = 3.58870e-3`, `phi2_sim = 7.15927e-4 Mpc^-3`; for `h_sim = 0.73`: `10.62355`, `4.49127e-3`, `8.95987e-4`. `n(>M)` is evaluated by fixed-node composite quadrature in `ln M` up to `120 Ms` onto a log-spaced table of at least 4096 points from `10^(ShamTargetLogMassFloor - 1)` to `120 Ms`, inverted by bisection with log-log interpolation. Reference values at `h_sim = 0.6774` (double-precision composite Simpson in `ln M`, 400,000 nodes): `n(>10^8) = 3.031993e-2`, `n(>10^9) = 1.162303e-2`, `n(>10^10) = 4.370059e-3`, `n(>10^11) = 3.398640e-4`, `n(>10^11.5) = 2.793662e-6 Mpc^-3`. The module's inversion must agree with the reference script of Slice 7 to `|delta log10 M*| <= 1e-4`.
- **Rank match (`post_snapshot`, output snapshots only).** Sort candidates by descending `ShamVpeak`, ties by ascending `UniqueGalaxyID`, in module-owned scratch; never sort the borrowed population; check ID uniqueness in an ID-sorted pass (the existing pattern). For zero-based rank `r`: `n_r = (r + 0.5) / BoxSize^3 * h_sim^3` in physical `Mpc^-3`. If `n_r > n(>10^ShamTargetLogMassFloor)` the candidate and every lower rank are masked (`ShamGhost = 1`, `StellarMass = 0`). Otherwise `M*_phys = n^-1(n_r)`, `StellarMass = M*_phys * h_sim / 1e10`, `ShamGhost = 0`. Above the published fit range the analytic double Schechter is evaluated as written; the README says so.
- **Fixture spot values** (`BoxSize = 100 Mpc/h`, `h = 0.6774`): rank 0 has `n_r = 1.554195e-7`, rank 5 `1.709615e-6 Mpc^-3`. On the real micro-Uchuu box with `ShamMinVpeak = 80`, the measured candidate density is between `0.0140` (rows with `Vmax >= 80` at snapshot 49) and `0.0233 Mpc^-3` (the all-ancestor upper bound), below the floor density `0.0303 Mpc^-3`, so masking is not expected to trigger on that run and the ranked population reaches down to roughly `10^8.4 Msun`; the masking rule is exercised by the unit and fixture tests. The audit line reports candidates, assigned and masked so the completeness statement is read from the run, not assumed.
- **Windows and failures.** `ShamTargetRedshiftMax` bounds assignment: init fails if any output snapshot's redshift exceeds it. Init also requires the module exactly once in `pre_timestep` as `process_full_halo` and exactly once in `post_snapshot` as `process_snapshot`, finite positive `BoxSize`, finite parameters with `phi1, phi2 > 0`, `ShamTargetHubble in (0, 2)`, `ShamMinVpeak > 0`. The strict parser accepts `nan`/`inf`, so every parameter is `isfinite`-checked before its range macro.
- **Properties.** `StellarMass` (float, `1e10 Msun/h`, output), `ShamVpeak` (float, km/s), `ShamMpeak` (float, `1e10 Msun/h`), `ShamGhost` (int, `range [0, 1]`, output; 1 = not a sample member). `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`, `StarFormationRate`, `ShamStellarMassNoScatter`, `ShamScatterDex` and `ShamOrphanAge` are removed.
- **Parameters:** `ShamTargetLogMstar`, `ShamTargetPhi1`, `ShamTargetAlpha1`, `ShamTargetPhi2`, `ShamTargetAlpha2`, `ShamTargetHubble`, `ShamTargetLogMassFloor`, `ShamTargetRedshiftMax`, `ShamMinVpeak`. No `parameter_units.yaml`: no parameter uses the `*_INTERNAL` loaders.

## Implementation Profiles and Run Preparation

| Slice | Recommended Developer | Effort | Reason |
|---|---|---|---|
| 1 | Claude Opus | high | Identity encoder and a reader vtable hook across four readers |
| 2 | Claude Opus | high | Byte-identical core refactor of the workspace growth seam |
| 3 | Claude Opus | high | The creation feature: dispatcher, marshal, fixture, contract |
| 4 | Claude Opus | high | New model package with stochastic physics and oracles |
| 5 | Claude Sonnet | high | HOD end-to-end tests, battery group, run files |
| 6 | Claude Sonnet | high | HOD figures and the shared pair-count helper |
| 7 | Claude Opus | high | SHAM replacement with a numerical inversion and reference oracle |
| 8 | Claude Sonnet | high | SHAM end-to-end tests, battery group, run files, coupling updates |
| 9 | Claude Sonnet | high | SHAM figures |
| 10 | Claude Sonnet | high | VISION amendment, guides, skills, changelog, pathway, CI |

PM executes each slice separately in plan order; no batching. Use the installed `opus` and `sonnet` aliases, record the resolved model versions, and pass the table's model and effort explicitly to `start-slice`. Reviewer recommendation: Claude Fable 5.1 (`claude-fable-5-1`), high effort, in fresh sessions for drift audit and code review. An unavailable model is a setup blocker, never permission to substitute silently.

Slices 1 to 5, 7, 8 and 10 require recorded human approval because they change shared interfaces, core execution, model physics, build or test entry points, or CI. Approval may be recorded for all of them before the run with PM's `approve` command; this document grants none. Slices 6 and 9 are plotting-only and run unattended.

Before PM initialisation, in order:

1. Commit this plan, the pathway update and the review's status note. PM's `init` refuses a tree dirty outside `.pm/` and binds the run to this file's bytes; any later edit stops the run.
2. Run `python3 ~/.claude/skills/project-manager/scripts/pm.py check-plan --plan docs/dev/MIMIC-HOD-SHAM-IMPLEMENTATION-PLAN.md --repo .` and resolve every warning.
3. Confirm the environment the Developer harness will actually use can run `make`, `mimic_venv`, HDF5 and the committed fixtures. Confirm the datasets the run files and tests name are mounted: `simulations/micro-uchuu-horizontal/snapshots`, `simulations/mini-millennium-horizontal/snapshots` and the eight `simulations/mini-millennium/snapshots` files (all verified present on 2026-10-04). Nothing in this plan needs Millennium, mini-Uchuu or Shin-Uchuu data.
4. Confirm `make tests-snapshot-global-identity` passes at the baseline; Slices 2 and 3 use it as their byte-identity guard and it refuses to run on a dirty runtime tree.

Work on the current branch unless the owner explicitly authorises a feature branch; PM refuses implicit `main`. No implementation session may edit this plan, change dependency manifests or licences, regenerate baselines, or push; remote CI is read by the owner after the run. Every slice uses one `MODEL`/`SIMULATION` pair per command sequence, exports the pair into the environment of any standalone runner (`tests/unit/run_tests.sh` falls back to the defaults otherwise), and restores the default pair's generated code before its final default-tier run.

Format-check note for every slice: run the full unmodified `./scripts/beautify.sh`, then `make check-format`; a `check-format` failure attributable only to files outside the repository tree is recorded as pre-existing and not fixed.

## Slice 1: Created-record identity and the largest-forest reader hook

### Intended Change

- Recommended Developer: Claude Opus; effort: high. No prerequisites beyond the planning baseline.
- Add the created-record identity encoder and its budget predicate to `src/include/galaxy_id.h`, the `MAX_CREATED_RECORDS_PER_HOST` constant, a vertical reader hook reporting the largest forest in a partition, and the per-driver startup evaluation and publication of `(unit, rows_per_unit)` that the encoder consumes. No record is created in this slice; existing IDs and existing runs are unchanged.

### Acceptance Criteria

- [ ] `MAX_CREATED_RECORDS_PER_HOST` is `1024` in `src/include/constants.h` with a comment stating it is an identity radix, not a physics cap.
- [ ] `galaxy_id.h` gains `mimic_created_record_space_fits(int64_t units, int64_t rows_per_unit)` returning true iff `units >= 0`, `rows_per_unit >= 0` and `1024 * units * rows_per_unit <= INT64_MAX` computed without intermediate overflow (zero units or zero rows fit), and `mimic_encode_created_galaxy_id(int64_t unit, int64_t row, int64_t rows_per_unit, int ordinal)` returning `-(1 + ordinal + 1024 * (row + rows_per_unit * unit))` with preconditions `0 <= row < rows_per_unit`, `0 <= ordinal < 1024`, `unit >= 0`, documented and asserted in debug builds. Every created ID is strictly negative; the positive tree encoding is untouched.
- [ ] `struct VerticalReader` gains `int64_t (*max_partition_unit_halos)(int partition)`: the largest halo count of any unit in that partition, or `-1` when the reader cannot know it without reading halo rows. `REQUIRE_READER_HOOK` applies. `lhalo_binary` and `lhalo_hdf5` answer from their headers; `consistent_trees_hdf5` answers from its per-forest counts if the file index carries them without reading rows, else `-1`; `consistent_trees_ascii` answers `-1`.
- [ ] `struct RecordIdentitySpace { int64_t unit; int64_t rows_per_unit; bool fits; }` is declared in `src/include/types.h`. The vertical driver computes the run-wide maximum over all partitions in the same scan that calls `count_partition_units`, on every rank identically; if any partition answers `-1` the run-wide value is "unknown" and `rows_per_unit = MimicConfig.UniqueGalaxyIDMultiplier`. It evaluates `fits` once with `units` = total forests, logs the three numbers and the verdict at INFO, and publishes `(unit = GlobalForestOffset + unit index, rows_per_unit, fits)` per unit. The horizontal driver computes `rows_per_unit = max over [0, info.snapshot_count) of snapshot_halo_count()` at open, evaluates `fits` with `units = info.snapshot_count`, logs the same line, and publishes `(unit = snapshot number, rows_per_unit, fits)` per snapshot. No run aborts on `fits == false` in this slice or any other; the verdict is consumed by Slice 3's creation call.
- [ ] Expected verdicts, stated from the packages' declared values rather than from runs this slice performs: full Millennium vertical `14329882 * 514194 * 1024 = 7.5e15` (fits), horizontal `64 * 18619466 * 1024 = 1.2e12` (fits); Shin-Uchuu ASCII vertical (`166547771 * 2e10 * 1024`) does not fit, and such a run must proceed with the verdict logged. On every committed fixture, and on the mini-Millennium data the unit test reads, the verdict is "fits".
- [ ] Unit tests cover: encoder bit patterns for small inputs, strict negativity, the exact predicate boundary (a `(units, rows_per_unit)` pair whose product times 1024 equals `INT64_MAX` rounded down, and the next integer up), zero units and zero rows, the unknown fallback through `test_enumerated_driver.c`'s synthetic reader, and the `lhalo_binary` hook on the mini-Millennium package data that `first_run.sh` fetches (`simulations/mini-millennium/snapshots`, eight files): the run-wide value equals the maximum `TreeNHalos` the test itself reads from the eight file headers, skipping with a stated reason only when that data is absent. The generic vertical and horizontal fixture runs produce byte-identical output to the baseline (no behaviour change).
- [ ] Required evidence: clean default build, `make check-generated`, all default test tiers, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`; no baseline refresh. `docs/DEVELOPER-GUIDE.md` "Adding a Vertical Reader" documents the new hook; `.agents/skills/mimic-simulations-and-readers/SKILL.md` lists it.

### Authorized Surface

- Files allowed to change:
  - `src/include/constants.h`
  - `src/include/galaxy_id.h`
  - `src/include/types.h`
  - `src/io/vertical/reader.h`
  - `src/io/vertical/binary.c`
  - `src/io/vertical/hdf5.c`
  - `src/io/vertical/read_ctrees_hdf5.c`
  - `src/io/vertical/read_ctrees_ascii.c`
  - `src/core/vertical_driver.c`
  - `src/core/horizontal_driver.c`
  - `tests/unit/test_created_record_identity.c` (new)
  - `tests/unit/test_enumerated_driver.c` (its synthetic reader gains the hook)
  - `docs/DEVELOPER-GUIDE.md` (the reader interface section)
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
- Functions/classes/components allowed to change: the new encoder and predicate, the new vtable hook and its four implementations, the partition scan and the two drivers' identity-space evaluation and publication, `test_enumerated_driver.c`'s synthetic reader. No change to `mimic_encode_unique_galaxy_id` or the positive encoding.
- Tests allowed or expected to change: the new unit test; existing driver tests only to supply the new hook.

### Explicit Non-Goals

- No record creation, no workspace or marshal change, no module API, no run YAML key, no output schema change, no change to any existing `UniqueGalaxyID`, no abort on a non-fitting space.

### Risk Flags

- Risky surfaces touched: reader vtable (shared type), driver startup, identity scheme.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `tests/unit/test_created_record_identity.c` registered by the unit glob; `test_enumerated_driver.c` hook supply.
- Commands to run: `make clean && make`, `make check-generated`, `make tests-unit`, `make tests-integration`, `make tests-scientific` (delegate the long tiers to a subagent that returns pass/fail), `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `make check-docs`, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the INFO line text on a vertical and a horizontal fixture run; confirm every reader's hook answer on its fixture by log inspection; confirm no output file bytes changed on the generic fixtures.

### Rollback Path

- Revert the slice commit; nothing downstream exists yet. Archive nothing: the slice adds files only.

## Slice 2: One workspace descriptor and growth function for both drivers

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slice 1.
- Replace the two driver-private workspace growth functions and the raw `(workspace, ngal)` hand-off with one core `struct FoFWorkspace` and one growth function in a new translation unit `src/core/fof_workspace.c`, carrying the galaxy pool and the identity space the creation operation will need, and route `process_halo_evolution()`, `execute_module_pipeline()`, `execute_phase()` and `marshal_workspace_to_output_buffer()` through it. Output bytes do not change. This is the seam Slice 3 grows through.

### Acceptance Criteria

- [ ] `src/core/fof_workspace.h` declares `struct FoFWorkspace { struct Halo *halos; int64_t count; int64_t capacity; struct GalaxyPool *pool; struct RecordIdentitySpace identity; }` and `void fof_workspace_reserve(struct FoFWorkspace *ws, int64_t required)` with the existing growth policy (x1.5, at least +1000, `MAX_HALO_ARRAY_SIZE` cap, `myrealloc_cat` in `MEM_HALOS`, zeroed tail) and `void fof_workspace_destroy(struct FoFWorkspace *ws)`; `src/core/fof_workspace.c` implements them. `ensure_fof_workspace_capacity()` and `horizontal_ensure_workspace_capacity()` are removed, not kept as wrappers.
- [ ] The vertical driver owns one `struct FoFWorkspace` in place of the `FoFWorkspace`/`MaxFoFWorkspace` globals: `src/core/allvars.c` and `src/include/globals.h` lose the globals and gain the descriptor's ownership note; `load_unit()` and `free_unit_halos()` in `src/io/vertical/interface.c` size, allocate and release it in place of the old globals; `pool` is `VerticalGalaxyPool`; `identity` is the unit's published space. The horizontal driver's `state->workspace`/`workspace_capacity` become one `struct FoFWorkspace` whose `pool` is the current generation's pool and whose `identity` is the snapshot's published space, set before each FoF group is processed.
- [ ] `process_halo_evolution(struct HaloInputView view, struct FoFWorkspace *ws, int64_t halonr)` replaces the old signature and reads the count from `ws`; `execute_module_pipeline(struct ModuleContext *ctx, struct FoFWorkspace *ws)` and `execute_phase(..., struct FoFWorkspace *ws)` read `ws->halos`/`ws->count` at the start of every module callback rather than from stack copies; `marshal_workspace_to_output_buffer(const struct FoFWorkspace *ws, ...)` takes the descriptor. The `process()` ABI and `struct ModuleContext`'s existing fields are unchanged; `ctx->central_galaxy` is still set per FoF.
- [ ] Every unit test that builds a workspace by hand (`tests/unit/test_event_buffer_growth.c`, `tests/unit/test_snapshot_module_contract.c`, `tests/unit/test_output_buffer.c`, `tests/unit/test_inheritance.c` and any other caller the build reveals) is adapted to the descriptor without weakening any assertion.
- [ ] `tests/unit/run_tests.sh` `CORE_SRCS` and `tests/unit/tools/build_topology_dump.sh` include the new unit; the Makefile's `find` picks it up with no edit.
- [ ] Byte identity: `make tests-snapshot-global-identity` passes (12 legs, both drivers, no provenance-only delta beyond those it already permits); the `sage16` physics baseline test and the core HDF5/binary baselines pass unchanged; the `mini-millennium-horizontal` real-data `halos-only` parity gate (`simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py`) passes.
- [ ] Required evidence: clean default build, `make check-generated`, all default tiers, `make tests-horizontal-v3`, `make tests-snapshot-global`, the identity test and the one real-data gate above; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/fof_workspace.h` (new)
  - `src/core/fof_workspace.c` (new)
  - `src/core/build_model.c`
  - `src/core/vertical_driver.c`
  - `src/core/horizontal_driver.c`
  - `src/core/halo_evolution.c`
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `src/core/output_buffer.c`
  - `src/core/output_buffer.h`
  - `src/core/allvars.c`
  - `src/include/globals.h`
  - `src/include/proto.h`
  - `src/io/vertical/interface.c`
  - `tests/unit/run_tests.sh`
  - `tests/unit/tools/build_topology_dump.sh`
  - `tests/unit/` (existing tests adapted to the descriptor only)
  - `docs/DEVELOPER-GUIDE.md` (FoF Workspaces and Per-Tree Memory Lifecycle sections)
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
- Functions/classes/components allowed to change: workspace ownership, allocation, release and growth, the four routed functions' signatures and their call sites, test harness workspace construction. No change to inheritance, the galaxy pool's own API, readers, output writers or any module.
- Tests allowed or expected to change: existing unit tests adapted to the descriptor; no new test is required beyond compiling and passing them.

### Explicit Non-Goals

- No record creation, no new module API, no staging, no change to marshal semantics, no output or schema change, no event change.

### Risk Flags

- Risky surfaces touched: core execution seam, global state ownership, shared function signatures.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: adapted unit tests only.
- Commands to run: `make clean && make`, `make check-generated`, the three default tiers via a subagent, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `python3 simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py` (real data, minutes), `make check-docs`, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: diff the run memory profile (`C`, `P`, `G`, `R`) of a fixture run before and after; the numbers must be identical.

### Rollback Path

- Revert the slice commit; Slice 1 stands alone. Archive nothing.

## Slice 3: The record-creation operation

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slices 1 and 2.
- Implement [The Record-Creation Contract](#the-record-creation-contract): `module_create_record()`, staging, the callback-boundary commit with cache refresh, the event rule, the marshal merge, created identity with the fits verdict, memory accounting, a `test_fixture` extension that creates records, the tests that prove the contract under both drivers, and the developer-guide contract section.

### Acceptance Criteria

- [ ] `int module_create_record(struct ModuleContext *ctx, int host_index, struct Halo **row)` is declared in `src/core/module_interface.h` with full Doxygen contract text and implemented in `src/core/module_registry.c`. It returns `>= 0` (the future logical index) on success; it returns `-1` with an `ERROR_LOG` naming the module and the reason when called outside a running `process_full_halo` callback (including from by-galaxy, per-event and snapshot callbacks and from `init()`), when `host_index` is outside `[0, committed count)`, when the host is not Type 0 or 1, when the host's galaxy is null, when the host's created-record ordinal would reach 1024, or when the workspace's `identity.fits` is false (that message carries `units`, `rows_per_unit`, the radix and the driver name). A failed call leaves no staged row and no pool allocation.
- [ ] `make_orphan()` is exported from `src/core/inheritance.c` through `src/core/inheritance.h` with its behaviour unchanged. The staged row is a struct copy of the host passed through `make_orphan()`, then `UniqueGalaxyID = mimic_encode_created_galaxy_id(ws->identity.unit, host HaloNr, ws->identity.rows_per_unit, ordinal)` and `galaxy = galaxy_pool_alloc(ws->pool)` followed by `init_galaxy_defaults()`. Field by field: `Type == 2`, `Mvir == 0`, `Len == 0`, `deltaMvir == -host Mvir`; `HaloNr`, `CentralHalo`, `CentralMvir`, `UniqueCentralGalaxyID`, `SnapNum`, `dT`, `Rvir`, `Vvir`, `Pos`, `Vel` equal the host's; for a Type 0 host `infallMvir`, `infallVvir`, `infallVmax` equal the host's `Mvir`, `Vvir`, `Vmax`; for a Type 1 host they equal the host's recorded infall values.
- [ ] Commit happens when the creating callback returns, before the next module runs and before pending events are delivered: staged rows are appended in creation order through `fof_workspace_reserve()`, `ws->count` grows, `ws->created_host` records each row's host, `ctx->central_galaxy` is recomputed from `ctx->central_index`, and the event-dispatch cache is refreshed. The next full-halo module in the same phase receives `(ws->halos, ws->count)` including the new rows; the by-galaxy pass of the same phase visits them; every later phase and substep sees them. Visibility is proved through the fixture's per-call execution log: a by-galaxy fixture in a later phase logs one `TEST_FIXTURE_EXEC` line per visited row, and the count rises by exactly the number of rows created in `pre_timestep` (tested).
- [ ] `module_emit_event()` rejects a `target_index` or `source_index` at or beyond the committed count with an `ERROR_LOG`, so an event cannot name a row created in the same callback; a later callback may target created rows (tested).
- [ ] `struct FoFWorkspace` gains `int64_t base_count` (set by `process_halo_evolution()` before the pipeline) and a capacity-tracked `int64_t *created_host` array indexed by `row - base_count`; the commit fills it. `marshal_workspace_to_output_buffer()` emits, per segment in order, the slice's surviving rows then that host's surviving created rows in creation order, locating each created row's segment from `created_host`, and `output_count` counts both. Created rows retired to Type 3 before marshal are dropped like any other. `FirstHalo`/`NHalos` write-back in both drivers is unchanged in code and correct in effect (tested against hand-built segments with created rows in the first, middle and last segment, and with a host in a later segment than a created row's position in the workspace tail).
- [ ] Inheritance: on the committed horizontal and vertical fixtures a created row is gathered into its host's descendant workspace at the next snapshot as a Type 2 row with its created `UniqueGalaxyID` unchanged and its galaxy deep-copied (tested through the real driver by reading HDF5 output at two consecutive output snapshots).
- [ ] Determinism: repeating a fixture run gives byte-identical created rows; the created ID of each row equals the encoder applied to the host's `HaloNr`, the unit and the ordinal read back from the run (tested); the per-host ordinal is counted within the FoF step in creation order.
- [ ] No-creation byte identity: with no module creating records, every output byte is unchanged; `make tests-snapshot-global-identity` passes and the `sage16` physics baseline and both core baselines pass unchanged.
- [ ] Memory: staging scratch and the per-host ordinal array are run-persistent, grow-to-high-water, `MEM_HALOS`, released at driver teardown, and every fixture run reports no leak. Created galaxies raise `G`, created rows raise `C`/`P`, nothing else changes in the profile (tested on a fixture run with and without creation).
- [ ] `src/module_system/test_fixture/` gains the optional integer parameter `TestFixtureCreateRecords`: absence means `0` (the fixture checks for the parameter's presence rather than failing `init()` on it), so every existing test input and every existing hand-built configuration is unchanged. When `> 0` the fixture creates that many records per Type 0 host on every `process()` call, writing `TestDummyProperty` on each, and logs `TEST_FIXTURE_CREATE: host=<UniqueGalaxyID> created=<n>` once per host. The fixture cannot know its dispatch mode; Slice 3's tests configure it as `process_full_halo` whenever the parameter is non-zero, and a by-galaxy configuration with the parameter set fails at its first creation call by the API's own gate. The existing fixture tests pass unchanged.
- [ ] Core unit tests (`tests/unit/test_record_creation.c`) drive `execute_module_pipeline()` with framework fixtures only, never production modules, and cover: every rejection path above including the `fits == false` path on a synthetic identity space; staged-row initialisation field by field for a Type 0 and a Type 1 host; visibility across modules, phases and substeps; the by-galaxy visit; the event rule both ways; the marshal merge cases; the memory high-water and leak check; and the encoder round trip. The integration test (`tests/integration/test_record_creation.py`) runs the committed fixture of the selected package (skipping with a stated configuration reason only for a package that has no fixture) and reads HDF5 output to prove: negative IDs only on created rows, created rows contiguous inside the host's segment order, `UniqueCentralGalaxyID` equal to the host's, inheritance at the next output snapshot, bitwise repeat identity, and leak-free runs. Under the default pair it exercises the vertical driver and under `MODEL=halos-only SIMULATION=mini-millennium-horizontal` the horizontal driver; the other driver's leg is not a separate case, so neither pair produces a SKIP.
- [ ] `tests/integration/test_unique_galaxy_id_encoding.py`'s docstring states that created records use the negative namespace and that the positivity assertion holds for the empty pipeline it runs.
- [ ] `docs/DEVELOPER-GUIDE.md` gains a "Record Creation Contract" section beside the snapshot callback contract carrying the contract text (API, initialisation, commit, marshal, identity with the fits verdict, memory, footguns); `src/core/module_interface.h` carries the same contract in Doxygen; `.agents/skills/mimic-modules/SKILL.md` and `.agents/skills/mimic-architecture-contract/SKILL.md` describe it (the latter adds the created-ID row to the invariants table).
- [ ] Required evidence: clean default build, `make check-generated`, all default tiers, `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration`, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, the `mini-millennium-horizontal` real-data `halos-only` gate; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/module_interface.h`
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `src/core/halo_evolution.c`
  - `src/core/inheritance.c` (export `make_orphan` only)
  - `src/core/inheritance.h` (declare `make_orphan` only)
  - `src/core/output_buffer.c`
  - `src/core/output_buffer.h`
  - `src/core/fof_workspace.c`
  - `src/core/fof_workspace.h`
  - `src/core/build_model.c` (teardown of creation scratch only)
  - `src/core/horizontal_driver.c` (teardown of creation scratch only)
  - `src/core/vertical_driver.c` (teardown of creation scratch only)
  - `src/util/run_profile.h` (notes only, if a term's definition text needs the created-row sentence)
  - `src/module_system/test_fixture/module_info.yaml`
  - `src/module_system/test_fixture/test_fixture.c`
  - `src/module_system/test_fixture/README.md`
  - `src/module_system/test_fixture/_tests/test_unit_test_fixture.c`
  - `src/module_system/test_fixture/_tests/test_integration_test_fixture.py`
  - `tests/unit/test_record_creation.c` (new)
  - `tests/integration/test_record_creation.py` (new)
  - `tests/integration/test_unique_galaxy_id_encoding.py` (docstring only)
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
- Functions/classes/components allowed to change: the new API and its gating state, the boundary commit in `execute_phase`, the event index validation, the marshal merge, creation scratch lifecycle, the `make_orphan` export, the fixture's optional parameter path. No change to the galaxy pool's API, readers, writers, any production module, the test harness, the test-input generator, or the positive identity encoding.
- Tests allowed or expected to change: the two new tests, the fixture's own tests, the one docstring.

### Explicit Non-Goals

- No creation from snapshot, by-galaxy or per-event callbacks; no event deferral; no new processing mode or phase; no `ModuleContext` processing-mode field; no run YAML key; no output schema change; no model package; no VISION edit (Slice 10).

### Risk Flags

- Risky surfaces touched: dispatcher execution, marshalling, identity, framework fixture.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed in the criteria. Register the new unit test by the glob and the new integration test by the glob; both pass in the default tiers under the default pair and under `MODEL=halos-only SIMULATION=mini-millennium-horizontal`.
- Commands to run: `make clean && make`, `make check-generated`, the three default tiers via a subagent, `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-integration`, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, the `mini-millennium-horizontal` real-data `halos-only` gate, `make check-docs`, `./scripts/beautify.sh`, `make check-format`; restore `MODEL=sage16 SIMULATION=mini-millennium` generated code last.
- Lint (differential, via the `lint` skill): required. Run differential `code-health` against the slice's starting commit and supply it as review evidence (structural change).
- Manual checks: inspect one fixture HDF5 output by hand for the segment order and IDs; check the three forbidden patterns of the snapshot contract are not reintroduced; confirm the run memory profile deltas.

### Rollback Path

- Revert the slice commit; Slices 1 and 2 remain valid and inert. Archive nothing.

## Slice 4: The `hod` model package and `hod_populate`

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slice 3.
- Create `models/hod/` with `model_properties.yaml`, the dual-mode module `hod_populate` implementing [HOD Model Specification](#hod-model-specification) (its binding equations are repeated below), its model-private RNG header, README files, unit tests with hand-checkable oracles, and the fixture run file the tests use.

### Acceptance Criteria

- [ ] `models/hod/model_properties.yaml` declares exactly one galaxy property, `HODGhost` (int, dimensionless, `init_value 0`, `range [0, 1]`, output; 1 = scaffold row outside the sample, 0 = sample member); `make MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal generate validate-modules lint-parameters check-generated` and the same under `MODEL=hod SIMULATION=mini-millennium` pass.
- [ ] `models/hod/modules/hod_populate/module_info.yaml` declares `supported_processing_modes: [process_full_halo, process_snapshot]`, every property it reads or writes (`Type`, `Mvir`, `Rvir`, `Vvir`, `Pos`, `Vel`, `UniqueGalaxyID`, `HODGhost`; the infall copy of `Vmax` is core's, inside `make_orphan()`, so the module does not declare `Vmax`), the ten parameters (`HODLogMmin`, `HODSigmaLogM`, `HODLogM0`, `HODLogM1`, `HODAlpha`, `HODSeed`, `HODConcA`, `HODConcLogMpivot`, `HODConcB`, `HODConcC`), its unit and integration tests and `docs.physics: README.md`; no events.
- [ ] `hod_populate_init()` requires the module exactly once in `post_timestep` as `process_full_halo`; when a `post_snapshot` phase is configured it must also contain the module exactly once as `process_snapshot` (the vertical driver has no snapshot phase, so a run file without `post_snapshot` is valid and runs without the audit). It loads the ten parameters with the existing macros, `isfinite`-checks each before its range check, and rejects `HODSigmaLogM <= 0`, `HODAlpha < 0`, `HODSeed < 0`, `HODConcA <= 0`, plus `nan`, `inf`, `1e400`, `1,0` and `abc` strings for any double parameter. It requires finite positive `BoxSize`.
- [ ] `hod_populate_process()` validates `ngal >= 1` and the Type 0 host at `ctx->central_index`, then on every call: retires every Type 2 row to Type 3; sets `HODGhost = 1` on every row; and, when the snapshot is in `output.snapshot_list` (an empty list means every snapshot), draws the host with `M = Mvir * 1e10` in `Msun/h`: `<Ncen> = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]`, central present iff `u0 < <Ncen>`; if present and `M > 10^HODLogM0`, `lambda = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha` and `Nsat ~ Poisson(lambda)` by inversion from one uniform with sequential cumulative search, else `Nsat = 0`. A present central sets the Type 0 row's `HODGhost = 0`; each satellite is created through `module_create_record()` with `HODGhost = 0`. Type 1 rows keep `HODGhost = 1`. A satellite is never created when the central is absent. `lambda >= 1024` or a drawn `Nsat > 1024` returns an error from the module with the host ID and `lambda` in the message.
- [ ] Placement, in double precision: `c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC` with `z = ctx->redshift`; `r_phys = Rvir * x / c` where `x` solves `m(x)/m(c) = u` with `m(x) = ln(1+x) - x/(1+x)` by bisection on `[0, c]` to `|m(x)/m(c) - u| <= 1e-12`; `cos theta = 2u - 1`, `phi = 2 pi u`; `r_com = r_phys * (1 + z)`; `Pos = host Pos + r_com n` with each component wrapped into `[0, BoxSize)` so that exactly `BoxSize` maps to `0`; `Vel = host Vel + (g1, g2, g3) * Vvir / sqrt(2)` with independent standard Gaussians. `Rvir` and `Vvir` are the host row's core fields.
- [ ] Spot values the unit tests reproduce: `<Ncen>(10^12.02) == 0.5` to 1e-12, `<Ncen>(1e14) == 1` to 1e-12, `lambda(1e13) = 0.457322`, `lambda(1e14) = 5.373959`, `lambda(1e15) = 61.842859` to 1e-6 absolute; `c(2e12, 0) = 5.71`, `c(1e14, 0) = 4.110765`, `c(1e12, 1) = 4.369568` to 1e-6; NFW inverse `x(0.5, 5) = 2.2166041757`, `x(0.1, 10) = 0.8223966449`, `x(0.9, 5.71) = 4.9207968140` to 1e-9; a position component exactly at `BoxSize` wraps to `0`, a component at `-1e-9` wraps to `BoxSize - 1e-9` within float rounding; at `z = 1` a satellite's comoving offset is twice its physical radius.
- [ ] Statistical tests over at least 20,000 independently keyed synthetic hosts, each stated with its derivation in the test: at `M = 10^12.02 Msun/h` the central frequency agrees with `0.5` within 4 binomial standard errors; at `M = 1e14 Msun/h` (where `<Ncen> = 1` to 1e-12, so the unconditional and central-conditional satellite laws coincide) the satellite count's mean and variance agree with `lambda = 5.373959` within 4 standard errors of each estimator; the satellite radial cumulative distribution at that mass agrees with `m(x)/m(c)` at ten quantiles within a Kolmogorov-Smirnov bound stated for the sample size; the one-dimensional velocity-offset variance agrees with `Vvir^2 / 2` within 4 standard errors.
- [ ] Determinism: repeating the draw for identical inputs gives bitwise-identical created rows; permuting rows within a FoF and permuting FoF order leave each host's draws unchanged (tested at unit level by driving `hod_populate_process()` directly on synthetic workspaces through the registered dispatch, with the fixture pattern of `sham_global_rank`'s unit test). The RNG is keyed by `(HODSeed, snapshot_number, host UniqueGalaxyID, draw index)`.
- [ ] The unit test covers the module's own output-snapshot test against `MimicConfig`: with a list naming only one snapshot, a call at another snapshot retires and resets but draws nothing; with an empty list every call draws.
- [ ] `hod_populate_process_snapshot()` on output snapshots computes, over the population with `V = BoxSize^3` in `(Mpc/h)^3`, `n_expected = sum over Type 0 rows of (<Ncen> + <Ncen> lambda) / V`, `n_realised = count(HODGhost == 0) / V`, the same two for satellites alone, and `<N(M)>` realised versus expected in 0.2 dex bins over the host mass range; it logs one `HOD audit z=<z> hosts=<n> n_gal expected=<x> realised=<y> f_sat expected=<a> realised=<b>` line at INFO and the per-bin lines at VERBOSE; it allocates only module-owned scratch released before return; on non-output snapshots it returns 0 without logging. It never writes any property.
- [ ] `models/hod/shared/hod_random.h` holds the RNG (splitmix64 counter generator, uniform in the open interval, Box-Muller Gaussian, Poisson by inversion), model-private, header-only, with a unit test of its determinism (same key and index give the same value; different indices differ) and of the open-interval guarantee; distributional checks live in the statistical tests above, not here.
- [ ] `models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml` targets the committed fixture through `simulation.config` as the sham example does, with `post_timestep: hod_populate: process_full_halo`, `post_snapshot: hod_populate: process_snapshot`, `snapshot_list: []`, HDF5 output, and the Mr < -20 and Duffy defaults (`12.02, 0.26, 11.38, 13.31, 1.06; 5.71, 12.301030, -0.084, -0.47`) with `HODSeed: 1`; its header states it is a framework demonstration on an incomplete fixture.
- [ ] `models/hod/README.md` documents the science scope, the five-parameter law and gating convention, placement including the physical-to-comoving conversion, RNG and determinism contract, the scaffold-and-ghost output contract, the mass-definition and cosmology caveats, the parameters with units, the audit log format, build/run/test commands and references (Zheng et al. 2005; Zheng, Coil & Zehavi 2007; Duffy et al. 2008; Berlind & Weinberg 2002). The module README documents processing contract, phases, properties, parameters and notes.
- [ ] Required evidence: module validation, parameter lint, generation freshness under both pairs above, the module's unit tests passing without skips under `MODEL=hod` with `mini-millennium` and with `micro-uchuu-ascii-horizontal` (run by path with the selectors exported), the default `sage16` tiers unaffected; style audit, differential lint, format and docs checks recorded.

### Authorized Surface

- Files allowed to change:
  - `models/hod/` (new package: `model_properties.yaml`, `README.md`, `input/hod_micro-uchuu-ascii-horizontal.yaml`, `shared/hod_random.h`, `shared/README.md`, `modules/hod_populate/`, `modules/_tests/hod_test_fixtures.h`)
- Functions/classes/components allowed to change: everything inside the new package. Core, scripts, Makefile, other models and simulations are read, never changed.
- Tests allowed or expected to change: the package's own unit tests (`modules/hod_populate/_tests/test_unit_hod_populate.c`, `shared/_tests/test_unit_hod_random.c` if the shared utility registry pattern is used) and nothing outside the package.

### Explicit Non-Goals

- No integration tests, battery group, Makefile target, real-data run file or plots (Slices 5 and 6); no stellar mass; no in-run `M_min` solve; no subhalo-position hybrid; no clustering; no core change.

### Risk Flags

- Risky surfaces touched: a new model prescription with stochastic draws and record creation.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the package unit tests above with the `TEST_RUN` count matching the battery's counting rule.
- Commands to run: `make MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=no generate validate-modules lint-parameters check-generated`, the same with `SIMULATION=mini-millennium`; `make MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes generate validate-build mimic` and `MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh models/hod/modules/hod_populate/_tests/test_unit_hod_populate.c`; `./mimic models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml` from the repository root and inspect its output and audit lines; restore the default pair and run its tiers via a subagent; `make check-docs`, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the audit line against a hand sum over the fixture's Type 0 hosts; check that every created row's `UniqueCentralGalaxyID` is its host's; account for every Type 2 retirement.

### Rollback Path

- Revert the slice commit and archive the package directory to `archive/` if any file must be removed; the core mechanism remains unaffected.

## Slice 5: HOD end to end: run files, battery group and integration tests

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slice 4.
- Add the HOD integration tests, the `hod` group of the snapshot-global battery with its `make tests-snapshot-global-hod` target, the real-data horizontal and the vertical run files, and the test-suite documentation.

### Acceptance Criteria

- [ ] `models/hod/modules/hod_populate/_tests/test_integration_hod_populate.py` copies a run file to a temp dir and runs `./mimic`, reading HDF5 per `UniqueGalaxyID`. Under `hod` x `micro-uchuu-ascii-horizontal` it uses the fixture run file and proves: every created row is Type 2 with a negative ID, `HODGhost == 0` and `UniqueCentralGalaxyID` equal to a Type 0 host that has `HODGhost == 0`; no satellite exists for a host with `HODGhost == 1`; Type 1 rows all have `HODGhost == 1`; no Type 2 row inherited from a previous snapshot survives in output; repeating the run is bitwise identical on the Galaxies datasets; changing `HODSeed` changes at least one draw; a run with `snapshot_list` naming only the last fixture snapshot writes created rows at that snapshot and is bitwise identical there to the all-snapshot run's last snapshot (earlier snapshots are not written, so their reset is proved by Slice 4's unit test); a conflicting configuration (module absent from `post_timestep`, or `post_snapshot` present without the module) is rejected at startup; each run is leak-free. Under `hod` x `mini-millennium` it uses the vertical run file on the package's eight files and proves the first four assertions and bitwise repeat identity. Under any other pair it raises the configuration `TestSkipped`, except for pure-Python cases.
- [ ] `tests/manual/run_snapshot_global_battery.py` gains `Group("hod", "hod", "micro-uchuu-ascii-horizontal", ...)` running the unit test by path and the integration test; `--only hod` maps to `make tests-snapshot-global-hod` and log `build/snapshot_global_hod_tests.log`; the Makefile target and `.PHONY`/help lines are added beside the existing ones. The battery's marker policy passes with zero SKIPs and matching declared counts.
- [ ] Run files: `models/hod/input/hod_micro-uchuu-horizontal.yaml` (the real 100 Mpc/h box, `snapshot_list: [49, 28, 23, 16, 12, 10, 8, 7]` as the halos-only sibling) and `models/hod/input/hod_mini-millennium.yaml` (vertical, no `post_snapshot`, HDF5 output, the eight local files). Each states the sample, the caveats and the driver it needs in its header. `./mimic models/hod/input/hod_micro-uchuu-horizontal.yaml` built for `MODEL=hod SIMULATION=micro-uchuu-horizontal` completes on the real dataset with a recorded wall clock, peak RSS, realised `n_gal` and `f_sat` from the audit, and an expected-versus-realised ratio within the Poisson scatter the audit's host counts imply; the numbers are recorded in the package README as the first real-data measurement. `./mimic models/hod/input/hod_mini-millennium.yaml` built for `MODEL=hod SIMULATION=mini-millennium` completes and its output contains created rows (the integration test above is its evidence).
- [ ] `scripts/fuzz_pipeline.py` can fuzz `hod` x `micro-uchuu-ascii-horizontal` from the fixture run file (its base-config lookup finds exactly one `*_micro-uchuu-ascii-horizontal.yaml` in `models/hod/input/`); the HOD startup contract errors are added to `_VALIDATION_PATTERN`.
- [ ] `tests/README.md` lists the new target and group with the same wording pattern as the sham entries.
- [ ] Required evidence: `make tests-snapshot-global` green (the `--only hod` target is a development convenience, not separate evidence); `make MODEL=hod SIMULATION=mini-millennium tests-integration` green; the default pair's tiers unaffected; the real-data run's recorded measurements.

### Authorized Surface

- Files allowed to change:
  - `models/hod/modules/hod_populate/_tests/test_integration_hod_populate.py` (new)
  - `models/hod/modules/hod_populate/module_info.yaml` (register the integration test)
  - `models/hod/input/hod_micro-uchuu-horizontal.yaml` (new)
  - `models/hod/input/hod_mini-millennium.yaml` (new)
  - `models/hod/README.md`
  - `tests/manual/run_snapshot_global_battery.py`
  - `Makefile` (the `tests-snapshot-global-hod` target, `.PHONY` and help lines only)
  - `scripts/fuzz_pipeline.py` (`_VALIDATION_PATTERN` and the docstring example only)
  - `tests/README.md`
- Functions/classes/components allowed to change: the battery's group table and `--only` mapping, the new target, the fuzzer's known-contract pattern.
- Tests allowed or expected to change: the new integration test.

### Explicit Non-Goals

- No module physics change, no core change, no plots, no CI edit (Slice 10), no Millennium or mini-Uchuu runs, no `mini-millennium-horizontal` HOD run file.

### Risk Flags

- Risky surfaces touched: build/test entry points.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: the integration test; the battery group.
- Commands to run: `make tests-snapshot-global`; `make MODEL=hod SIMULATION=micro-uchuu-horizontal generate validate-modules && make MODEL=hod SIMULATION=micro-uchuu-horizontal -j$(sysctl -n hw.ncpu)` then the real-data run from the repository root; `make MODEL=hod SIMULATION=mini-millennium tests-integration`; `python3 scripts/fuzz_pipeline.py --model hod --simulation micro-uchuu-ascii-horizontal` for a handful of seeds; restore the default pair and run its tiers via a subagent; `make check-docs`, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the real-data audit lines and the run memory profile; confirm every SKIP in the default tiers has a stated reason unrelated to this slice.

### Rollback Path

- Revert the slice commit; Slice 4's package still builds and its unit tests still run by path.

## Slice 6: HOD figures and the shared correlation-function helper

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slice 5 (real-data output to plot against).
- Add the model-neutral pair-count helper and its unit test to the plotting engine, and the `models/hod/plots/` package: the occupation figure with the analytic law overlaid, the satellite phase-space check, the real-space correlation function, and two copied halo diagnostics.

### Acceptance Criteria

- [ ] `plot/mimic-plot/output_utils.py` gains `periodic_pair_counts(positions, box_size, r_edges)` (cell-grid, minimum image, numpy only, returns pair counts per bin excluding self-pairs, each pair counted once) and `correlation_function(positions, box_size, r_edges)` returning `xi = DD / RR_analytic - 1` with `RR_analytic = N (N - 1) / 2 * shell_volume / box_size^3`, plus Poisson error bars. Input domain, validated with explicit errors: finite coordinates of shape `(N, 3)` inside `[0, box_size)`, `r_edges` strictly increasing and non-negative with `r_edges[-1] <= box_size / 2`, `box_size > 0`; `N < 2` returns zero counts and `xi` of NaN with zero-count error bars rather than dividing by zero. A unit test in `plot/mimic-plot/tests/test_correlation_function.py` checks: a brute-force O(N^2) count on 500 random points equals the grid count bin for bin; `xi` of a uniform random sample is consistent with zero within 3 Poisson errors in every bin; a pair placed exactly across the periodic boundary is counted at the minimum-image separation; each domain violation raises.
- [ ] `models/hod/plots/figures/hod_occupation.py`: from the sample (`HODGhost == 0`) and the Type 0 hosts, plot realised `<Ncen>`, `<Nsat>` and `<Ntot>` versus `log10 Mvir` (converted to `Msun/h`) with binomial or Poisson error bars, overlaid with the analytic curves computed from the run's parameters read through `params["EnabledModules"]["parameters"]`, binned through `get_profile_axes` and `make_bin_edges`; it returns `(path, None)` or `(None, reason)`.
- [ ] `models/hod/plots/figures/hod_satellite_profile.py`: a two-panel figure: the stacked distribution of `r_com / ((1 + z) Rvir_host)` for created satellites (minimum-image distance to the host, converted back to physical) against the prediction formed as the satellite-weighted average over hosts of each host's own NFW cumulative profile at its concentration; and the histogram of each velocity-offset component divided by its host's `Vvir / sqrt(2)` against the unit Gaussian. The snapshot redshift comes from the package's `a_list` through `SnapshotRedshiftMapper`.
- [ ] `models/hod/plots/figures/hod_correlation_function.py`: `xi(r)` of the sample on logarithmic bins from `0.1` to `20 Mpc/h` (profile-overridable within the helper's domain) with error bars, using the shared helper, annotated with the realised number density and satellite fraction.
- [ ] `halo_mass_function` and `spatial_distribution` are copied from `models/halos-only/plots/figures/` unchanged (the two that give the occupation and clustering figures their context); `figures/__init__.py` registers the five figures with `PLOT_REQUIREMENTS` naming `HODGhost` (and `Pos`, `Rvir`, `Vel`, `Vvir` where read); `profiles/default.yaml` lists them; `profiles/micro-uchuu-horizontal_plot_profile.yaml` carries the axes.
- [ ] Running `mimic-plot.py --param-file models/hod/input/hod_micro-uchuu-horizontal.yaml` on the Slice 5 output produces every figure without a skip; the figures are inspected and, in the occupation figure, every bin with at least 50 hosts has its realised value within 3 standard errors of the analytic law, with the count of bins and the largest deviation recorded in the slice summary.
- [ ] The HOD README gains the plot list; `plot/mimic-plot/README.md` lists the `hod` registry size and the correlation-function helper.

### Authorized Surface

- Files allowed to change:
  - `plot/mimic-plot/output_utils.py`
  - `plot/mimic-plot/tests/test_correlation_function.py` (new)
  - `plot/mimic-plot/tests/test_plotting.sh` (add the new unit test to its list)
  - `plot/mimic-plot/README.md`
  - `models/hod/plots/` (new)
  - `models/hod/README.md`
- Functions/classes/components allowed to change: the two new helpers, the new figures and profiles, the registry.
- Tests allowed or expected to change: the new plotting unit test.

### Explicit Non-Goals

- No `wp(rp)`, no observational clustering overlay, no scipy or new dependency, no change to the engine's reading or profile code, no SHAM figure, no copy of `spin_distribution`, `velocity_distribution` or `hmf_evolution`.

### Risk Flags

- Risky surfaces touched: none beyond a shared plotting helper.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: the correlation-function unit test.
- Commands to run: `mimic_venv/bin/python plot/mimic-plot/tests/test_correlation_function.py`; `mimic_venv/bin/python plot/mimic-plot/mimic-plot.py --param-file models/hod/input/hod_micro-uchuu-horizontal.yaml` on the Slice 5 output; `./scripts/beautify.sh`, `make check-format`, `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: look at every figure; confirm axis labels carry units and the sample definition.

### Rollback Path

- Revert the slice commit; the helper and figures are additive.

## Slice 7: Replace the SHAM package: `sham_rank_match`

### Intended Change

- Recommended Developer: Claude Opus; effort: high. No dependency on Slices 1 to 6 (the module creates no records and uses only the existing module ABI); it is ordered after them to keep the core work contiguous.
- Remove `sham_assign_stellar_mass`, `sham_global_rank`, `models/sham/modules/_tests/sham_test_fixtures.h`, `models/sham/parameter_units.yaml` and the three legacy run files from the tree (copies archived under `archive/models-sham-legacy/`); rewrite `model_properties.yaml`; add the dual-mode module `sham_rank_match` implementing [SHAM Model Specification](#sham-model-specification) (its binding equations are repeated below) with its reference oracle and unit tests, the fixture run file and the README files. Update the one generator comment that cites a removed property.

### Acceptance Criteria

- [ ] After the slice `models/sham/modules/` contains only `sham_rank_match/` and `_tests/sham_test_fixtures.h` (rewritten for the new parameters); `git ls-files models/sham` lists no legacy file; `archive/models-sham-legacy/` holds byte-identical copies of every removed file (gitignored).
- [ ] `models/sham/model_properties.yaml` declares exactly `StellarMass`, `ShamVpeak`, `ShamMpeak` (unchanged definitions) and `ShamGhost` (int, dimensionless, `init_value 0`, `range [0, 1]`, output, description stating 1 = not a sample member); `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal generate validate-modules lint-parameters check-generated` passes, and so does the same under `SIMULATION=mini-millennium`. `scripts/generate_properties.py:626`'s comment no longer cites `ShamOrphanAge`.
- [ ] `sham_rank_match/module_info.yaml` declares `supported_processing_modes: [process_full_halo, process_snapshot]`, properties `Type`, `UniqueGalaxyID`, `Mvir`, `Vmax`, `StellarMass`, `ShamVpeak`, `ShamMpeak`, `ShamGhost`, the nine parameters (`ShamTargetLogMstar`, `ShamTargetPhi1`, `ShamTargetAlpha1`, `ShamTargetPhi2`, `ShamTargetAlpha2`, `ShamTargetHubble`, `ShamTargetLogMassFloor`, `ShamTargetRedshiftMax`, `ShamMinVpeak`), unit and integration tests, `docs.physics: README.md`; no events.
- [ ] `sham_rank_match_init()` requires the module exactly once in `pre_timestep` as `process_full_halo` and exactly once in `post_snapshot` as `process_snapshot`; `isfinite`-checks every parameter before its range check and requires `phi1, phi2 > 0`, `ShamTargetHubble in (0, 2)`, `ShamMinVpeak > 0`, finite positive `BoxSize`; fails if any output snapshot's `ZZ` exceeds `ShamTargetRedshiftMax`; rejects the strings `nan`, `inf`, `infinity`, `1e400`, `1,0`, `abc` for any double parameter; and builds the converted target table once with `log10 Ms_sim = ShamTargetLogMstar + 2 log10(h_obs / h_sim)`, `phi_i_sim = phi_i (h_sim / h_obs)^3`, `h_obs = ShamTargetHubble`, `h_sim = Hubble_h`: `n(>M) = integral_M^(120 Ms) exp(-M'/Ms) [phi1 (M'/Ms)^alpha1 + phi2 (M'/Ms)^alpha2] dM'/Ms` by fixed-node composite quadrature in `ln M'` onto a log-spaced table of at least 4096 points from `10^(ShamTargetLogMassFloor - 1)` to `120 Ms`, inverted by bisection with log-log interpolation. `cleanup()` frees it.
- [ ] Converted-parameter and table values the unit tests reproduce: at `h_sim = 0.6774`, `log10 Ms_sim = 10.68851`, `phi1_sim = 3.58870e-3`, `phi2_sim = 7.15927e-4` to five significant figures, and `n(>10^8) = 3.031993e-2`, `n(>10^9) = 1.162303e-2`, `n(>10^10) = 4.370059e-3`, `n(>10^11) = 3.398640e-4`, `n(>10^11.5) = 2.793662e-6 Mpc^-3` to a relative `1e-4`; at `h_sim = 0.73`, `10.62355`, `4.49127e-3`, `8.95987e-4`.
- [ ] `sham_rank_match_process()` (`pre_timestep`) on every row, in this order: a Type 2 row becomes Type 3; a Type 0/1 row updates `ShamVpeak = max(ShamVpeak, Vmax)` and `ShamMpeak = max(ShamMpeak, Mvir)`, where the double `max` must be `<= FLT_MAX` before the float cast (`Mvir = FLT_MAX` stored exactly, `1e39` fails the FoF step with the `UniqueGalaxyID` and value); `ShamGhost = 1` and `StellarMass = 0`. It writes nothing else and never allocates.
- [ ] `sham_rank_match_process_snapshot()` on an output snapshot: validates every entry (Type 0 or 1, since the `pre_timestep` callback retired every Type 2 before marshal and a Type 2 or 3 entry here means it did not run; non-null galaxy; positive unique `UniqueGalaxyID` checked in an ID-sorted scratch pass; finite non-negative peaks) and fails the snapshot with the `UniqueGalaxyID` and value on any violation with no partial assignment; takes as candidates the Type 0/1 rows with `ShamVpeak >= ShamMinVpeak`; sorts them in module-owned scratch by descending `ShamVpeak`, ties by ascending `UniqueGalaxyID`; for zero-based rank `r` computes `n_r = (r + 0.5) / BoxSize^3 * h_sim^3` in physical `Mpc^-3`; if `n_r > n(>10^ShamTargetLogMassFloor)` masks that candidate and every lower rank (`ShamGhost = 1`, `StellarMass = 0`); otherwise sets `StellarMass = n^-1(n_r) * h_sim / 1e10` and `ShamGhost = 0`; releases scratch before return on every path; and logs one `SHAM audit z=<z> candidates=<n> assigned=<a> masked=<m>` line at INFO. On a non-output snapshot it returns 0 without ranking or logging. It never sorts or resizes the borrowed population, retains no pointer, and never indexes by `CentralHalo`.
- [ ] Numerical contract: `models/sham/modules/sham_rank_match/_tests/sham_rank_match_reference.py` computes `n(>M)` and its inverse in double precision by composite Simpson in `ln M` with 400,000 nodes from the published parameters and the stated conversions (the plan's reference values were produced this way and the 1e-4 tolerance is far above double rounding), and defines a table of at least eight `(h_sim, rank density)` cases including the fixture's rank 0 (`1.554195e-7`) and rank 5 (`1.709615e-6 Mpc^-3`) densities at `h = 0.6774`, one case at `h = 0.73`, one just inside and one just outside the floor boundary, and one deep in the exponential tail; the C unit test embeds that table between `SHAM_DECIMAL_REFERENCE_BEGIN/END` markers (the marker names follow the house precedent) and checks the module's `log10 M*` within `1e-4` of the reference and mask/assign outcomes exactly; a pure-Python integration case regex-parses the C table and requires equality with the reference, so the table cannot drift.
- [ ] Unit tests also cover: init placement and parameter rejections, the redshift-window rejection, the hand-solvable tie order (two equal `ShamVpeak`, lower ID ranks first), permutation and repeat bitwise identity, cross-FoF global ranking (a higher proxy in another FoF shifts ranks), Type 2 retirement and Type 1 retention, peak persistence through a masked snapshot, the `Mvir = FLT_MAX` and `1e39` peak cases, empty and all-ineligible populations, no net allocation growth across snapshots, registered dispatch through `execute_post_snapshot()`, and leak checks.
- [ ] `models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml` (replacing the `sham_global_` file, so the fuzzer's single-match lookup holds) targets the committed fixture through `simulation.config`, configures `pre_timestep: sham_rank_match: process_full_halo` and `post_snapshot: sham_rank_match: process_snapshot`, `snapshot_list: []`, the Baldry+12 parameters (`10.66, 3.96e-3, -0.35, 0.79e-3, -1.47`) with `ShamTargetHubble 0.7`, `ShamTargetLogMassFloor 8.0`, `ShamTargetRedshiftMax 0.2` (the fixture's six scale factors `0.83728` to `0.99951` span `z = 0.1943` to `0.0005`, so every snapshot is assigned; the header says so and that the real-data file uses `0.1`), and `ShamMinVpeak 80`; its header states the fixture is not a complete volume.
- [ ] `models/sham/README.md` is rewritten: scientific status (a calibrated-target rank match, cross-sectional, zero scatter, resolved-only, no observational parity claim), the target with its conventions and the `h` rescaling, candidate policy, masking rule, parameters, output contract (`ShamGhost`), build/run/test, follow-ups (scatter, orphans), references (Conroy et al. 2006; Vale & Ostriker 2006; Behroozi et al. 2010; Reddick et al. 2013; Baldry et al. 2012). The module README follows the house pattern.
- [ ] Required evidence: validation, lint, freshness under both pairs; the unit tests passing without skips under `MODEL=sham` with both pairs (by path for the horizontal one, selectors exported); the fixture run from the repository root with its output and audit line inspected; the default `sage16` tiers unaffected; style audit, differential lint, format and docs checks recorded.

### Authorized Surface

- Files allowed to change:
  - `models/sham/` (the whole package: removals, the rewritten `model_properties.yaml` and `README.md`, the new `modules/sham_rank_match/`, the rewritten `modules/_tests/sham_test_fixtures.h`, the new `input/sham_micro-uchuu-ascii-horizontal.yaml`, the removed files)
  - `scripts/generate_properties.py` (the one comment at line 626 only)
- Functions/classes/components allowed to change: everything inside the package; one comment in the generator. Core, Makefile, battery, fuzzer, docs and plots are not changed here (Slices 8 and 9).
- Tests allowed or expected to change: the package's own tests; the legacy tests are removed with their modules.

### Explicit Non-Goals

- No scatter, no orphan mode, no tabulated-file target, no clustering, no plots, no battery or Makefile change, no documentation outside the package, no vertical run file (the model is horizontal-only), no change to any other model.

### Risk Flags

- Risky surfaces touched: a model prescription with a numerical inversion; wholesale removal of a shipped package's modules. Between this slice and Slices 8 and 9 the battery's `sham` group, the fuzzer's pattern and `models/sham/plots/` (which reads removed properties) are stale; the slice summary records all three.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the module unit test with its reference table, the reference script, the integration test file carrying the pure-Python table check (the fixture-driven integration cases are Slice 8).
- Commands to run: `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=no generate validate-modules lint-parameters check-generated`, same for `mini-millennium`; `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes generate validate-build mimic` and `MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh models/sham/modules/sham_rank_match/_tests/test_unit_sham_rank_match.c`; `./mimic models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml`; restore the default pair and run its tiers via a subagent; `make check-docs`, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: hand-check the fixture rank 0 mass against the reference table; confirm `rg -n 'sham_global_rank|sham_assign_stellar_mass|ShamOrphanAge|ShamScatterDex|ShamStellarMassNoScatter' models/sham/modules models/sham/model_properties.yaml models/sham/input scripts/generate_properties.py` returns nothing; list the remaining stale references for Slices 8 and 9 in the slice summary.

### Rollback Path

- Revert the slice commit; the archived copies under `archive/` are the local record. Downstream slices do not exist yet.

## Slice 8: SHAM end to end: run files, battery, couplings and integration tests

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slice 7.
- Extend the SHAM integration test with its fixture-driven cases, retarget the battery's `sham` group and the fuzzer's pattern to the new module, add the real-data run file, and update every repository coupling that named the removed modules or the vertical SHAM pair.

### Acceptance Criteria

- [ ] `models/sham/modules/sham_rank_match/_tests/test_integration_sham_rank_match.py` (extended from Slice 7's table check) runs the fixture run file from a temp copy and proves, reading HDF5 per `UniqueGalaxyID`: every assigned row's `log10 StellarMass` matches the reference for its rank within `1e-4`; every masked row and every Type 0 or 1 row below the floor has `ShamGhost == 1` and `StellarMass == 0`; no Type 2 row is in output; the galaxies of the fixture's snapshot-4 halos 0 and 2 (FoF centrals whose halos merge into snapshot-5 halo 0, so core demotes them to Type 2 at snapshot 5) are present in the snapshot-4 output and absent from the snapshot-5 output, which proves the retirement path end to end; repeat runs are bitwise identical; cross-FoF tie handling holds on a derivative fixture edited through h5py in the test's temp dir (the existing pattern); startup rejects an output snapshot outside the redshift window and a missing `pre_timestep` entry; each run is leak-free. Configuration `TestSkipped` under any pair other than `sham` x `micro-uchuu-ascii-horizontal`, except pure-Python cases.
- [ ] `tests/manual/run_snapshot_global_battery.py`'s `sham` group names the new unit and integration test files; `make tests-snapshot-global` passes with zero SKIPs and matching counts (the `--only sham` target is a development convenience); the Makefile comment above the targets describes the new module.
- [ ] `scripts/fuzz_pipeline.py`'s `_VALIDATION_PATTERN` replaces the removed mutual-exclusion message with the new module's startup contract messages and its docstring example names the new run file; fuzzing `sham` x `micro-uchuu-ascii-horizontal` for a handful of seeds reports only known-contract rejections.
- [ ] `models/sham/input/sham_micro-uchuu-horizontal.yaml` targets the real box with `snapshot_list: [49]` (z = 0.0005, the only epoch inside the target's window) and `ShamTargetRedshiftMax: 0.1`; its header explains that a cross-sectional low-redshift target gives one output epoch and that the peak history is still accumulated over every processed snapshot; the run completes with recorded wall clock, peak RSS, and candidate, assigned and masked counts from the audit line (expected between 44,978 and 75,117 candidates with `ShamMinVpeak 80`, and no masking if the candidate density stays below `0.0303 Mpc^-3`; whatever is measured is what the README records), and the resulting stellar mass function at z = 0 is compared numerically (counts per 0.25 dex bin against the target's expectation for the same bins and volume) with the comparison recorded in the package README as the first real-data measurement; it is a target-recovery check on the ranked population, not an observational parity claim.
- [ ] Every repository reference to the removed modules, the vertical SHAM run files, `parameter_units.yaml` as a SHAM example, or `make MODEL=sham SIMULATION=mini-millennium` is updated: `tests/README.md:29-30, 62, 174`, `docs/USER-GUIDE.md:59, 303-306, 398, 707, 717`, `docs/DEVELOPER-GUIDE.md:5`, `plot/mimic-plot/README.md:83, 162`, `.agents/skills/mimic-run-and-operate/SKILL.md:45-47`, `.agents/skills/mimic-build-and-env/SKILL.md:93`, `.agents/skills/mimic-config-and-flags/SKILL.md:35, 142`, `.agents/skills/mimic-modules/SKILL.md:121`, `.agents/skills/mimic-properties/SKILL.md:78`, `.agents/skills/mimic-validation-and-qa/SKILL.md:45, 85`, `.agents/skills/mimic-debugging-playbook/SKILL.md:78-81`, `.agents/skills/mimic-sam-reference/SKILL.md:132-134, 154`, `.agents/skills/mimic-change-control/SKILL.md:67`, `.agents/skills/mimic-architecture-contract/SKILL.md` (W6 rewritten to describe the new state), and `simulations/micro-uchuu-ascii/README.md:37` (the historical seed note marked as referring to a removed module). `rg -n 'sham_global_rank|sham_assign_stellar_mass|ShamOrphanAge|ShamLogM1|sham_mini-millennium|sham_millennium' --hidden -g '!.git' -g '!archive' -g '!output' -g '!docs/dev' -g '!CHANGELOG.md' -g '!models/sham/plots' .` returns nothing (the plots are Slice 9's).
- [ ] Required evidence: `make tests-snapshot-global` green; the default pair's tiers unaffected; `make check-docs` green; the real-data run's recorded measurements.

### Authorized Surface

- Files allowed to change:
  - `models/sham/modules/sham_rank_match/_tests/test_integration_sham_rank_match.py` (extended)
  - `models/sham/input/sham_micro-uchuu-horizontal.yaml` (new)
  - `models/sham/README.md`
  - `tests/manual/run_snapshot_global_battery.py`
  - `Makefile` (comments and help text of the snapshot-global targets only)
  - `scripts/fuzz_pipeline.py` (`_VALIDATION_PATTERN` and the docstring example only)
  - `tests/README.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md` (line 5 and the sham mentions only)
  - `plot/mimic-plot/README.md`
  - `simulations/micro-uchuu-ascii/README.md` (the one historical note)
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-build-and-env/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-properties/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-sam-reference/SKILL.md`
  - `.agents/skills/mimic-change-control/SKILL.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
- Functions/classes/components allowed to change: the battery group, the fuzzer pattern, documentation text naming SHAM. No code outside the package and the two scripts.
- Tests allowed or expected to change: the extended integration test.

### Explicit Non-Goals

- No module physics change, no plots (Slice 9), no CI edit, no CHANGELOG or VISION or pathway edit (Slice 10), no vertical SHAM.

### Risk Flags

- Risky surfaces touched: test entry points, documentation of record across many files.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: the extended integration test; the battery group.
- Commands to run: `make tests-snapshot-global`, the real-data run built for `MODEL=sham SIMULATION=micro-uchuu-horizontal`, the fuzzer for a handful of seeds, the default pair's tiers via a subagent, `make check-docs`, the `rg` sweep above, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the real-data audit line and the SMF comparison table; read every edited documentation paragraph once in context.

### Rollback Path

- Revert the slice commit; Slice 7's package still builds and its unit tests run by path, with the battery's `sham` group stale until re-pointed.

## Slice 9: SHAM figures

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slices 6 and 8.
- Bring `models/sham/plots/` to the new model: the stellar mass function against its configured target, the sample-filtered stellar-to-halo relations and satellite fraction, a correlation function by stellar-mass threshold through the shared helper, and the registry and profiles that match the new properties.

### Acceptance Criteria

- [ ] `stellar_mass_function.py` filters on `ShamGhost == 0`, overlays the configured double Schechter (read from `params["EnabledModules"]["parameters"]`, converted exactly as the module does) as a line and keeps the Baldry et al. (2008) band; the red/blue split and any `StarFormationRate` or `WhichIMF` branch are removed (the model has neither).
- [ ] `sham_stellar_halo_relation.py` and `sham_satellite_fraction.py` filter on `ShamGhost == 0`, bin through `get_profile_axes` and `make_bin_edges`, and label the satellite population as Type 1 only.
- [ ] `sham_correlation_function.py` plots `xi(r)` for at least two stellar-mass thresholds (profile-configurable, defaults `10^10` and `10^10.5 Msun`) through the shared helper within its domain, annotated with each sample's number density.
- [ ] `smf_evolution.py`, `stellar_mass_density_evolution.py`, `hmf_evolution.py` and `halo_occupation.py` are removed from the package (the shipped run writes one epoch, so evolution figures have nothing to show, and the occupation figure belongs to the HOD package); the README states this. The four halo diagnostics `halo_mass_function`, `spin_distribution`, `velocity_distribution` and `spatial_distribution` stay unchanged.
- [ ] `figures/__init__.py`, `profiles/default.yaml` and new `profiles/micro-uchuu-horizontal_plot_profile.yaml` match the surviving figures (`EVOLUTION_PLOTS` is empty); `PLOT_REQUIREMENTS` names `ShamGhost` wherever it is read.
- [ ] Running `mimic-plot.py --param-file models/sham/input/sham_micro-uchuu-horizontal.yaml` on the Slice 8 output produces every registered figure without a skip; the stellar mass function figure shows the ranked population tracking the target line above the masking floor; `plot/mimic-plot/README.md` lists the `sham` registry size. `rg -n 'StarFormationRate|WhichIMF|ShamOrphanAge' models/sham/plots` returns nothing.

### Authorized Surface

- Files allowed to change:
  - `models/sham/plots/`
  - `models/sham/README.md` (plot list)
  - `plot/mimic-plot/README.md` (registry size sentence)
- Functions/classes/components allowed to change: the package's figures, registry and profiles.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No engine change, no new helper, no observational SMF data beyond Baldry 2008 and the configured target, no HOD figure.

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `mimic_venv/bin/python plot/mimic-plot/mimic-plot.py --param-file models/sham/input/sham_micro-uchuu-horizontal.yaml` on the Slice 8 output; `./scripts/beautify.sh`, `make check-format`, `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: look at every figure; confirm the target line and the ranked SMF use the same `h` conversion.

### Rollback Path

- Revert the slice commit; the engine is untouched.

## Slice 10: Publish the contract and close out

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slices 1 to 9.
- Make the permanent documents, skills and CI describe the shipped behaviour: a narrow VISION amendment for record creation, the user and developer guides, the README and AGENTS tree, the changelog, the skills sweep, the CI step, and the pathway's record of this focus and its follow-ups.

### Acceptance Criteria

- [ ] `docs/VISION.md` Principle 4 gains one requirement bullet and one "in practice" sentence: modules may create galaxy records through one core creation operation from full-halo callbacks; created records join the FoF workspace when the creating callback returns and are marshalled inside their host's output segment; core owns their defaults, identity, linkage and memory and the module owns their physics and lifetime; the snapshot scope remains topology-immutable. Principle 5 gains one sentence: created records are accounted in the galaxy pool and output buffer like any row. No other VISION change.
- [ ] `docs/USER-GUIDE.md`: the package list names `hod`; the physics-pipeline section has a short "Creating galaxy records" paragraph pointing at the developer guide; the snapshot-wide section's example names `sham_rank_match`; configuration recipes gain one HOD and one SHAM recipe; the plotting section names the new figures; troubleshooting lists the creation-API messages, including the identity-budget message and what it means for Shin-Uchuu ASCII and full Uchuu under the vertical driver.
- [ ] `docs/DEVELOPER-GUIDE.md`: the "Record Creation Contract" section from Slice 3 is cross-linked from the module lifecycle and processing-mode sections; the `ModuleContext` reference table and parameter sections mention the API; a short "Adding a New Model Package" subsection lists the mandatory file and the conventions this plan followed.
- [ ] `AGENTS.md` (the `.claude/CLAUDE.md` target) lists the four model packages as `sage16, sham, hod, halos-only` in its tree line; `README.md` gains one sentence naming the `hod` and `sham` packages beside its existing `sage16` and `halos-only` mentions.
- [ ] `CHANGELOG.md` Unreleased gains three bullets in the house style: the record-creation operation and created identity (with the vertical reader hook and the startup verdict), the `hod` package (sample, placement, caveats, first real-data measurement), and the `sham` replacement (target, candidates, masking, first real-data measurement, what was removed). Nothing is rewritten in released sections.
- [ ] Skills sweep, each verified against the final code: `mimic-architecture-contract` (data flow adds the commit step; invariants table carries the created-ID row; W6 describes the new SHAM; weak points updated), `mimic-modules` (creation API, dual-mode module pattern with phase placement checks), `mimic-properties` (the `int` flag pattern; no `parameter_units.yaml` example from `sham`), `mimic-plots-and-analysis` (registry counts, the correlation helper), `mimic-validation-and-qa` (the `hod` and `sham` battery groups and CI step), `mimic-config-and-flags` (new parameters are model-local; `parameter_units.yaml` sentence), `mimic-debugging-playbook` (creation-API and identity-budget messages; the new modules' init errors), `mimic-run-and-operate` (HOD and SHAM run commands), `mimic-sam-reference` (HOD and SHAM paragraphs), `mimic-simulations-and-readers` (the hook, if Slice 1 left anything stale), `mimic-scientific-method` (what the two packages claim and do not).
- [ ] `.github/workflows/ci.yml`'s `horizontal-v3` job gains a step `make tests-snapshot-global` after the existing steps; the slice's evidence for it is the same command run locally, since implementation sessions do not push. The owner pushes after the run and reads CI.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the current-focus box records this plan as complete with the measured outcomes; the "Next focus" section becomes a closed record pointing at the permanent docs; Recorded residuals gain: SHAM intrinsic scatter with deconvolution; orphan-inclusive SHAM with a disruption criterion; `wp(rp)` with an SDSS overlay; pair identity as the escape beyond the scalar budget, with the vertical Shin-Uchuu ASCII and full-Uchuu ceilings named; event deferral to the callback boundary; an output-time filter for ghost scaffold rows at production scale; HOD mass-definition and cosmology caveats; the SHAM seed-cast residual is removed because its module is gone. The plan inventory lists this plan for archiving.
- [ ] `make check-docs` passes; every edited Markdown file has no hard-wrapped prose; `make tests summary` shows no SKIP whose reason is this plan's work.

### Authorized Surface

- Files allowed to change:
  - `docs/VISION.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `README.md`
  - `AGENTS.md`
  - `CHANGELOG.md`
  - `.github/workflows/ci.yml`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-properties/SKILL.md`
  - `.agents/skills/mimic-plots-and-analysis/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-sam-reference/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-scientific-method/SKILL.md`
- Functions/classes/components allowed to change: documentation text and one CI step. No code.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code change, no plan edit, no archive move of this plan (the owner archives it after acceptance), no release tag, no push.

### Risk Flags

- Risky surfaces touched: CI configuration, VISION.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`, `make tests-snapshot-global` locally (the CI step's command), `make tests summary` via a subagent, `./scripts/beautify.sh`, `make check-format`.
- Lint (differential, via the `lint` skill): required (the YAML and Markdown are linted files).
- Manual checks: read the VISION diff against the shipped behaviour sentence by sentence; read each skill's edited section in context.

### Rollback Path

- Revert the slice commit; code slices are unaffected and the docs revert to naming the previous state.

## Next Chat Prompts

### Mode A — Checkpointed alternative

```text
Plan file: docs/dev/MIMIC-HOD-SHAM-IMPLEMENTATION-PLAN.md
Slices this session: Slice 1

Read the full plan. Stop before coding if any receipt is incomplete or baseline drift
changes its meaning. Work on the current branch; do not create a branch without my instruction.
Use orchestrator as the controlling skill. Keep implementation, tests and Git local.
Use the slice's recommended Developer model/effort and a fresh read-only Claude Fable 5.1
high-effort Reviewer for each independent review. Do not substitute or self-audit.
Restate the frozen surface and non-goals. Obtain approval for the flagged slice first.
Apply scoped-implementation and style-guide write mode. Run its validation and differential lint.
Apply drift-audit with the Reviewer, and report the authorization gate and provenance.
Only after it passes, run differential code-health for structural changes and independent
code-review. Fix findings in scope and repeat the affected gates at the final tree.
Ask me before committing; then use commit.
After the selected slice is committed, use handoff and stop with the next slice recorded.
Confirm the plan, selected slice, branch and model/effort before beginning.
```

### Mode B — Supervised execution

```text
Plan file: docs/dev/MIMIC-HOD-SHAM-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness claude model opus effort high initially
Reviewer: harness claude model claude-fable-5-1 effort high

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline and resolve any drift against the plan's anchors first. Use the current
branch explicitly; do not create a new branch unless I authorize it.
Record human approval for each flagged slice before starting it; neither this launcher nor
the plan grants those approvals.
Ask for commit authorization before launch unless I have already explicitly granted it
for this run; once granted, PM Developer slice commits follow the PM contract.
Keep PM_RUN_TOKEN private to the PM seat. Preflight the datasets, mimic_venv and HDF5 the
plan names without changing source data.

For each slice in order:
1. Launch a fresh Developer, explicitly passing that receipt's model and effort to
   start-slice (Opus high for 1, 2, 3, 4 and 7; Sonnet high for 5, 6, 8, 9 and 10).
2. Wait with one long observe --wait, asynchronously when possible. Do not poll a healthy
   session. Stop for actual dialogs and human gates; nudge only a genuine stall.
3. Check the mechanical floor, then the frozen authorization contract and actual evidence.
   Run differential lint and a style-guide audit; investigate differential code-health for
   structural changes. Required test evidence in Acceptance Criteria binds both seats.
4. For elevated slices commission fresh independent drift-audit; read and judge it and report
   authorization before commissioning fresh code-review. Both must cover the exact final
   commit. Rerun validation as required. Inspect actual output, audit lines and skip counts.
5. Record reviewer/developer judgments and accept, steer or stop from repository evidence.
   A contract defect stops the run; do not amend this plan or invent missing science policy.
   Carry resolved decisions and outstanding evidence in PM notes.

Confirm the plan, branch, Developer/Reviewer models and effort, approvals and first slice.
After all slices are decided, read the PM report and report its verified total elapsed time,
accepted commits/evidence, audit provenance, plan defects, stops and residual limitations.
```
