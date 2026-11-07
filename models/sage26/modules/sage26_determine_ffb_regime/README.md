# `sage26_determine_ffb_regime`

Classifies every unmerged row of the FoF group as a feedback-free-burst (FFB) row (`FFBRegime` 1) or a normal row (`FFBRegime` 0), once per snapshot, after the infall budget and before the substeps, with SAGE26's Li et al. (2024) FFB fraction and SAGE26's own SplitMix64 draw.

## SAGE26 Source

A port of SAGE26's `determine_and_store_ffb_regime` at `EnhancedStarFormationOn` 1 (`src/model_regimes.c:188-240`), which SAGE26 calls once per FoF group after `determine_and_store_regime` and `infall_recipe` and before the substeps, only when `EnhancedStarFormationOn >= 1` (`src/core_build_model.c:407-427`). For every row whose `mergeType` is not positive (`:213`), with no `Type` or CGM-regime restriction (`:215-217`):

1. `draw = halo_uniform(MostBoundID, snapshot)` (`:225`), SAGE26's static SplitMix64 hash (`:151-159`) of the row's 64-bit `MostBoundID` and the current snapshot (`halo_snapnum`, `src/core_build_model.c:407`): `x = (uint64_t)id ^ ((uint64_t)(uint32_t)snap * 0x9E3779B97F4A7C15)`, `x += 0x9E3779B97F4A7C15`, two multiply-xorshift steps, `x ^= x >> 31`, and `((double)(x >> 11) + 0.5) * (1.0 / 9007199254740992.0)`;
2. `f_ffb = calculate_ffb_fraction(Mvir, Zcurr)` (`:232`, `:321-341`) of the row's float `Mvir` widened (`:229`) at the current redshift `Zcurr = ZZ[halo_snapnum]` (`src/core_build_model.c:408`): the threshold mass `pow(10.0, 0.8 + log10(h) - 6.2 log10((1 + z) / 10))` (`:298-313`, Li et al. 2024 eq. 2, in 1e10 Msun/h), 0 when `Mvir <= 0.0 || Mvir_ffb <= 0.0` (`:332`), and otherwise `x = log10(Mvir / Mvir_ffb) / 0.15` and `1 / (1 + exp(-x))` (`:337-339`, eq. 3);
3. `FFBRegime = (draw < f_ffb) ? 1 : 0` (`:236-240`), a strict comparison.

The arithmetic is `shared/sage_ffb_regime.h`'s, every statement one C expression with SAGE26's operand order and types. Where SAGE26's object computes a form other than its source text the package writes the object's form, shown from the disassembly (the oracle README's "The c13 Group"): the log term's product fused into its sum (an explicit `fma()`), and `exp(-x)` as `exp(log10(Mvir / Mvir_ffb) / -0.15)`, which equals the source form for every non-NaN `x` and carries a NaN's sign as SAGE26 does. `pow(10.0, ...)` is written as `pow`: SAGE26's object calls `__exp10` there, and the two agree at every snapshot redshift of mini-Millennium and every fixture redshift (the executed check in the oracle README).

**SAGE26's quirks, kept.** Satellites and orphans are classified like centrals (the source comment's "each central galaxy", `src/model_regimes.c:164`, is stale and not ported); the CGM regime does not enter. A NaN `Mvir` or threshold passes the guards and gives a NaN fraction, which compares false (0); an infinite `Mvir` gives the fraction 1. `z = -1` gives `z_norm` 0 and an infinite threshold, so every finite `Mvir` has fraction 0 through `x = -inf`; a redshift below -1 gives a NaN threshold. The draw keeps its arithmetic exactly as written, with no endpoint correction (the largest 53-bit value rounds to a deviate of exactly 1.0).

**The discarded `rand()`.** SAGE26 calls `rand()` once per classified row and discards the value (`src/model_regimes.c:224`), so that its other draws keep their places in the libc stream. The package makes no `rand()` call: it has no libc stream to keep in step, so the discarded draw has no counterpart in the package; a `// SAGE parity` comment marks the point in `sage26_determine_ffb_regime.c` where SAGE26 makes it. SAGE26's useful draw is the hash, which the package reproduces bit for bit; unlike the CGM classifier's (`sage26_determine_regime`, whose deviate is the package's keyed draw, `shared/sage_keyed_draw.h`, in place of SAGE26's libc `rand()`, `src/model_regimes.c:94`) it is SAGE26's own and is not replaced.

**The key.** The package keys the draw on the core's halo `MostBoundID`, which equals SAGE26's `galaxies[p].MostBoundID` on every Type 0 and Type 1 row (both take the (sub)halo's at joining: `src/core_build_model.c:254`; `src/core/inheritance.c`, `apply_descendant_properties`) and on a galaxy created this snapshot (`src/model_misc.c:99`). On a Type 2 row the two can differ (Mimic's created rows share their host's `MostBoundID`), but `Mvir` is 0 there in both, so `f_ffb` is 0 and the row is 0 whatever its draw. The current redshift and snapshot come from the module context (`ctx->redshift`, `ctx->snapshot_number`), never from a row's stored snapshot.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`, once
- Receives the full FoF workspace

`init()` refuses any placement but exactly one `pre_timestep` entry as `process_full_halo`: an entry in a substep phase or `post_timestep`, a second entry, another processing mode, or an entry before `sage26_determine_regime` or `sage26_prepare_infall_budget` where those are configured in `pre_timestep`, each naming what it refuses. The check names modules and their order, never a substep phase's name. The run files place it last in `pre_timestep`, after `sage26_prepare_infall_budget`, as SAGE26 calls it after `infall_recipe`. With `EnhancedStarFormationOn` 1 `init()` also refuses, naming it, a configuration without `sage26_ffb_star_formation` in a substep phase as `process_by_galaxy`: an `FFBRegime` 1 row bypasses the ordinary star-formation chain (`src/model_starformation_and_feedback.c:1004-1008`), so without the FFB transaction it would form no stars. At 0 that check does not apply.

**Rows.** SAGE26 skips only rows with `mergeType > 0`; the package's mark of such a row is `Type` 3, and a row without a galaxy is skipped as every dispatcher does. At `pre_timestep` neither occurs (the core discards an inherited `Type` 3 row at joining, and SAGE26's join guard `continue`s before `ngal++`, `src/core_build_model.c:241-244`). A row in the core's first-snapshot boundary state (stored `SnapNum` -1, `dT` 0) is classified like any other: the classifier reads no snapshot-indexed table.

## Properties

- Reads: `Type`, `Mvir` (narrowed to SAGE26's float), `MostBoundID` (the simulation package's halo property)
- Writes: `FFBRegime` (only with `EnhancedStarFormationOn` 1; with 0 it keeps its creation value 0). It carries over between snapshots (no `init_repeat`), as SAGE26's member does.

The run's `Hubble_h` is read from the module context's parameters.

## Parameters

- `EnhancedStarFormationOn`: required, with no default; a missing key fails `init()`. 0 (no classification; SAGE26's configurations R0-R5) or 1 (the Li et al. 2024 classifier; configuration R6 and SAGE26's default, `input/millennium.par:58`, `src/core_read_parameter_file.c:106`). 2, the Boylan-Kolchin (2025) acceleration criterion (`src/model_regimes.c:241-288`), is not ported and is refused at `init()` with every other value, naming the parameter (`mimic_sage_validate_enhanced_star_formation_on`, `shared/sage_ffb_regime.h`). The run files hold 1 (the package is on R6, SAGE26's default configuration); value 1 also switches on SAGE26's FFB star formation, `sage26_ffb_star_formation`, which an `FFBRegime` 1 row takes in place of the ordinary chain.

## Test Seam

`sage26_determine_ffb_regime.h` declares `sage26_determine_ffb_regime_set_test_seam()`. With a seam installed, `process()` still forms SAGE26's draw, but compares the seam's deviate when the seam gives one, and reports every classified row's values (the keys, the redshift, the widened `Mvir`, the fraction's locals, the fraction, the draw's 64 bits and deviate, the deviate compared and the `FFBRegime` written); `NULL` removes it. It exists so a test can place a deviate exactly at the fraction (the tie `draw == f_ffb`, which no `MostBoundID` can be chosen to give). A run never installs one.

## Tests

- `_tests/test_unit_sage26_determine_ffb_regime.c`: platform-independent behavioural tests, each expectation a literal, a frozen draw vector or SAGE26's own expression written in the test: `EnhancedStarFormationOn` 0 writes nothing; Types 0, 1 and 2 are classified and a Type 3 row keeps a seeded `FFBRegime`; the CGM `Regime` does not change the result; the result reads the narrowed `Mvir`, the current redshift (not the stored snapshot's) and a draw keyed on `MostBoundID` and the current snapshot (not `UniqueGalaxyID` or the stored `SnapNum`); through the seam a deviate equal to the fraction gives 0 and one ulp below gives 1; an initial-boundary row; the parameter's refusals and every misplacement; at `EnhancedStarFormationOn` 1 the refusal of a configuration without `sage26_ffb_star_formation` by galaxy in a substep phase, accepted at 0.
- `_tests/test_unit_sage26_determine_ffb_regime_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`, written by `oracle_c13`): the threshold and fraction fixtures through the shared helpers directly, and each classifier fixture's rows through `process()` with the production draw, asserting every recorded field and local bit for bit against the SAGE26 function-level oracle (`../_tests/oracle/README.md`, "The c13 Group"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- `_tests/test_integration_sage26_determine_ffb_regime.py`: an integration test that loads and runs the module in Mimic and checks that with `EnhancedStarFormationOn` 0 it is a no-op; that with 1 it writes its domain, `FFBRegime`; that its `init()` refusals reach the run's exit status; and the cross-module guards described under the Processing Contract.
- The helpers themselves are tested by `shared/_tests/test_unit_sage_ffb_regime.c`.

## References

- Li et al. (2024), the feedback-free-burst threshold mass and fraction (eqs. 2-3), as SAGE26 cites it (`src/model_regimes.c:46-50`, `:167`)
- Steele, Lea & Flood (2014), SplitMix64, as SAGE26 cites it (`src/model_regimes.c:146`)
