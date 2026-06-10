# Mimic sage16 vs Original SAGE: Divergence Investigation and Parity Fixes

**Date**: 2026-06-11
**Scope**: Why mimic's sage16 model output diverged slightly from the original SAGE (`sage-code/sage-model/`) on mini-Millennium, what was fixed, and what residual differences remain.

---

## Executive Summary

Mimic-sage16 and original SAGE were compared galaxy-by-galaxy on identical mini-Millennium trees (HDF5 output, 36,531 galaxies at z=0, matched by position/Len/Type). The investigation found that **the divergence was not caused by mimic's modular architecture** — the per-galaxy execution order of mimic's galaxy-major phase loop reproduces SAGE's evolve loop faithfully. Instead, six concrete implementation-level differences were identified and fixed. After the fixes, **97.9–100% of galaxies are bit-identical per property** (previously 4–20%), median relative differences are exactly zero, and the surviving differences are at the float ULP level (99th percentile ~10⁻⁶) plus a handful of galaxies (~0.1%) where a chaotic borderline merger/disruption decision flips.

The dominant cause was almost comically small: **original SAGE writes √2 as the literal `1.414`**, while mimic used `1.414213562`. This 3×10⁻⁴ offset in every disk scale radius perturbed the star formation threshold of every galaxy at every substep and accounted for the bulk of the visible divergence in the plots (e.g. GasFraction.png).

All three test tiers (unit, integration, scientific) pass after the changes, and `make check-format`, `check-generated`, `check-docs`, and `validate-modules` are clean.

---

## Methodology

1. **Reference data**: original SAGE output at `/Volumes/Internal/results/sage-model/millennium/reference/model_0.hdf5` (git SHA `1b83152`, all 64 snapshots); mimic output regenerated from `models/sage16/input/sage16_mini-millennium.yaml`.
2. **Configuration audit**: run parameters, cosmology, units, and snapshot lists confirmed identical (SAGE HDF5 `Header/Runtime` vs mimic YAML/metadata).
3. **Galaxy matching**: per snapshot, galaxies matched by (Posx, Posy, Posz, Len, Type) — unique in both outputs; 100% match rate.
4. **Code audit**: every sage16 module and shared header compared line-by-line against the corresponding original SAGE source (`core_build_model.c`, `model_*.c`, `core_cool_func.c`), including constants, thresholds, float/double precision of every intermediate, and intra-substep operation ordering.
5. **Iterative fix-and-measure**: each fix was followed by a full mini-Millennium rerun and re-comparison against the unchanged reference.

## What Already Matched Perfectly

These were verified and required no changes:

- **Core tree processing**: Mvir, Rvir, Vvir, Vmax, VelDisp, infallMvir/Vvir/Vmax, dT, positions, velocities, spins — *bit-identical* for every galaxy at every output snapshot, including SAGE's quirky "Rvir/Vvir only update when Mvir grows" rule and orphan inheritance.
- **Galaxy counts and types**: identical (36,531 at z=0; 31,739 centrals / 4,792 satellites) before any fix.
- **Cooling tables**: data files byte-identical; interpolation code identical.
- **All physics formulas**: cooling regimes, AGN modes (empirical/Bondi/cold-cloud), Eddington limit, SN reheating/ejection, reionization filtering-mass fitting formulas, merger mass ratios, starburst coefficients, disk instability criterion, dynamical friction merger clock, quasar wind energetics (including c in cm/s).
- **Execution order**: mimic's `execute_phase()` runs by-galaxy modules **galaxy-major** (all modules per galaxy, then the next galaxy), which reproduces SAGE's per-galaxy loop semantics — including the cross-galaxy coupling through the central's hot gas reservoir. The `satellite_mergers` phase with immediate event dispatch reproduces SAGE's in-loop merger handling (merge transfer → BH growth → quasar wind → starburst → post-merger instability recheck → bulge conversion, in SAGE's exact order).

## Root Causes of Divergence

| # | Cause | Class | Effect size |
|---|-------|-------|-------------|
| A | Disk radius constant: SAGE uses literal `1.414` (twice), mimic used `1.414213562f` | Constant mismatch | 3.02×10⁻⁴ on every DiskScaleRadius → perturbs SF threshold of every galaxy every substep; **dominant cause** |
| B | Reionization H(z): mimic used `100.0 * hubble_h` (=73); SAGE uses code-unit Hubble ≈ 100 (h-free, since lengths are Mpc/h) | **Genuine bug** | Mchar overestimated by 1/h (~37%) → excess infall suppression for small halos near z≈7–8 |
| C | Metallicity helper: float arithmetic (SAGE: double); missing `metals > 0` guard (could return negative Z); epsilon cutoff instead of `gas > 0` | Precision + guard mismatch | ~10⁻⁷ noise on every metal transfer; wrong sign possible for ULP-negative metal reservoirs |
| D | Threshold mismatches: `EPSILON_SMALL` (10⁻¹⁰) guards where SAGE uses `> 0.0`, and 10⁻¹⁰ where SAGE's metal-yield branch uses `> 1.0e-8`; Krumholz & Dekel scale used own-halo Mvir instead of SAGE's central Mvir | Edge-case mismatch | Rare discrete branch flips |
| E | Metal-yield ordering: mimic applied the disk-SF yield *before* the disk-instability chain; SAGE applies it *after* `check_disk_instability()` | **Ordering deviation** | Instability starbursts saw already-enriched cold gas → systematic metals tail (p99 up to ~10⁻¹ in MetalsColdGas), present from the earliest snapshot |
| F | float32 transport: inter-module properties (CoolingGas, InfallingGas, NewStellarMass, SN masses, Rcool, CoolingLambda, HaloBaryonFraction, UnstableDiskGasFraction) and core dT stored as float; SAGE carries the equivalent values as double locals through a timestep | Precision (architecture-adjacent) | ~10⁻⁷ noise floor on everything, chaotically amplified over 64 snapshots |

Output-convention differences that are **not** physics divergence (left as-is): `TimeOfLastMajorMerger` units (mimic code units vs SAGE Myr, sentinel 0 vs negative), SFR/Cooling/Heating/OutflowRate output normalization (verified to match at unity ratio).

## Fixes Implemented

All changes are confined to the sage16 model package plus one core property-precision change; none alter the module-system architecture.

1. **Fix A** — `sage_set_disk_scale_radius.c`: use SAGE's literal `1.414` and double-precision arithmetic (float storage unchanged). Test reference implementation updated to match.
2. **Fix B** — `sage_reionization.c`: `HubbleZ = ctx->params->Hubble * sqrt(...)` (code-unit H0, no h factor). This is both a parity fix and a physics correction.
3. **Fix C** — `shared/metallicity.h`: double-precision, SAGE-exact semantics (`gas > 0 && metals > 0`, cap at 1.0, never negative). Callers aligned to SAGE's per-call-site precision (double in stripping and BH growth; float retained in `add_infall_to_hot` where SAGE itself uses float).
4. **Fix D** — exact SAGE thresholds: `> 0.0` gates in apply-cooling, radio-mode heating (including the `rheat < rcool` suppression branch), reincorporation; `> 1.0e-8` metal-yield branch; Krumholz & Dekel scale now uses the FOF central's Mvir. The satellite-stripping metal transfer now reproduces SAGE's asymmetry exactly (central receives the *unclamped* `strippedGas * metallicity` while the satellite loses the clamped amount).
5. **Fix E** — new module **`sage_apply_metal_enrichment`** (`models/sage16/modules/sage_apply_metal_enrichment/`): the disk-SF yield moved out of `sage_apply_star_formation_supernova` into this module, configured *after* the disk-instability chain in `galaxy_physics` (both input YAMLs updated). `NewStellarMass` is now consumed (zeroed) by the enrichment module instead of the apply step. Init-time dependency checks enforce the SAGE ordering, and the apply step warns if the enrichment module is missing.
6. **Fix F** — transport properties promoted to `type: double` in `models/sage16/model_properties.yaml` (InfallingGas, CoolingGas, Rcool, CoolingLambda, NewStellarMass, SupernovaReheatedMass, SupernovaEjectedMass, HaloBaryonFraction, UnstableDiskGasFraction) and `dT` in `src/core/core_properties.yaml`; premature `(float)` casts removed in module code so reservoir updates use SAGE's `float += double` arithmetic. Properties SAGE itself stores as float (Rheat, MergTime, DiskScaleRadius, all reservoirs) deliberately stay float.

Tests updated to the new contracts: metallicity edge cases (SAGE-exact thresholds, negative-metals guard), reionization test fixtures (set `MimicConfig.Hubble`), reincorporation ULP-negative metals tolerance (matches SAGE behavior), and the SF/SN apply suite now exercises the two-module apply→enrichment pipeline.

## Quantitative Results

Fraction of matched galaxies with bit-identical values (z=0, snapshot 63, 36,530 matched):

| Property | Before | After A–D | After A–F (final) |
|----------|--------|-----------|-------------------|
| StellarMass | 0.20 | 0.86 | **0.996** |
| ColdGas | 0.17 | 0.86 | **0.981** |
| HotGas | 0.54 | 0.74 | **0.905** |
| EjectedGas | 0.38 | 0.89 | **0.970** |
| BulgeMass | 0.76 | 0.90 | **0.988** |
| BlackHoleMass | 0.82 | 0.89 | **0.997** |
| MetalsColdGas | 0.24 | 0.86 | **0.990** |
| DiskScaleRadius | 0.00 | 1.00 | **1.000** |

Median relative differences fell from ~10⁻⁴–3×10⁻⁴ (systematic) to exactly 0; the 99th percentile fell from up to ~0.8 (metals) to ~10⁻⁶. At the earliest output snapshot (16, z≈5.3) most properties are now 100% bit-identical across all 3,908 galaxies.

## Remaining Differences and Why

1. **ULP-level noise** (~10⁻⁷ relative, the float epsilon): a few remaining places where SAGE's arithmetic uses unrounded doubles that mimic stores as float at a boundary — e.g. SAGE's `estimate_merging_time()` consumes `get_virial_mass/radius/velocity()` return values at double precision, while mimic's merger clock reads the float-stored workspace values. Eliminating these would require core changes (double virial properties) with no statistical benefit.
2. **Discrete decision flips** (~0.1% of galaxies, including one galaxy in the z=0 count: 36,530 vs 36,531): when a satellite's MergTime or baryon-ratio test sits exactly on a threshold, a 1-ULP difference flips merge-vs-disrupt or this-substep-vs-next, producing a large difference for that one galaxy (visible mainly in ICS, which receives disrupted satellites). This is chaotic amplification, not a systematic bias; both codes are equally "correct" at these boundaries.
3. **Output conventions** (intentional): `TimeOfLastMajorMerger`/`TimeOfLastMinorMerger` units and never-merged sentinel; mimic normalizes Cooling/Heating/OutflowRate by each galaxy's own dT where SAGE uses the first galaxy's deltaT for all galaxies in the FoF (a SAGE quirk that mimic improves on; identical in practice for same-snapshot galaxies).

Statistically, the two codes now produce indistinguishable populations: every plotted distribution (mass functions, gas fractions, metallicity relations, BH–bulge, SFR densities) is built from per-galaxy values that are bit-identical for ≳98% of galaxies and within 10⁻⁶ for the rest.

## Vision Compliance

No VISION.md principle was compromised:

- **Physics-agnostic core**: untouched except for one precision annotation (`dT` float→double in core property metadata), which is model-independent.
- **Runtime modularity**: improved — the metal yield is now an explicit pipeline step (`sage_apply_metal_enrichment`) whose ordering contract is declared in the input YAML and enforced at init, rather than being implicit inside the apply step.
- **Metadata as source of truth**: precision changes were made in property YAML, not hand-edited generated code.
- **One coherent processing model**: unchanged; the investigation *confirmed* the galaxy-major phase loop reproduces SAGE's semantics.
- **Validation/fast failure**: the new module ships with init-time ordering checks and a configuration warning for silent yield loss.

The one philosophically interesting choice: mimic now deliberately reproduces SAGE's truncated `1.414` constant and its stripping metal asymmetry. These are documented in code comments as SAGE-parity decisions; a future non-parity model package is free to use exact constants.

## Files Changed

- `models/sage16/modules/sage_set_disk_scale_radius/` — Fix A (+ unit test)
- `models/sage16/modules/sage_reionization/` — Fix B (+ unit test fixtures)
- `models/sage16/shared/metallicity.h` — Fix C (+ shared unit test)
- `models/sage16/shared/sage_agn_physics.h`, `shared/sage_starburst_physics.h` — Fix C/D
- `models/sage16/modules/sage_apply_cooling/`, `sage_radio_mode_heating/`, `sage_reincorporation/`, `sage_satellite_stripping/`, `sage_apply_infall/`, `sage_prepare_infall_budget/`, `sage_calculate_cooling_budget/` — Fix D/F
- `models/sage16/modules/sage_apply_star_formation_supernova/` — Fix D/E (+ unit tests)
- `models/sage16/modules/sage_apply_metal_enrichment/` — **new module** (Fix E)
- `models/sage16/model_properties.yaml`, `src/core/core_properties.yaml` — Fix F
- `models/sage16/shared/time_parity.h` — Fix F (double comparisons)
- `models/sage16/input/sage16_mini-millennium.yaml`, `sage16_millennium.yaml` — pipeline updated for Fix E

## Verification

- Full mini-Millennium rerun compared per-galaxy against the untouched original-SAGE reference after each fix stage (results above).
- `make tests-unit` (32/32), `make tests-integration` (all pass), `make tests-scientific` (all pass).
- `make validate-modules`, `make check-generated`, `make check-format`, `make check-docs` all clean.
- Pre-fix outputs archived under `archive/parity-test/` for re-comparison.

## Recommendations

1. Regenerate the published results in `/Volumes/Internal/results/mimic/sage16-mini-millennium/` (and the full-Millennium run, which also gains the reionization H(z) fix) from the fixed build, then re-make the plots — GasFraction.png and the metallicity plots should now overlay the originals.
2. If a future model package wants "corrected" physics (exact √2, symmetric stripping metals), fork sage16 rather than editing it — sage16's role is faithful Croton et al. (2016) behavior.
