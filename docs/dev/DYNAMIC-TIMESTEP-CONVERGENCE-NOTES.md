# Dynamic-Timestep Convergence Notes: What Happens as SubSteps → ∞

**Status:** Analysis, informational — not an implementation plan.
**Scope:** How `SubSteps` (fixed and dynamic interpretations) affects the sage16 physics pipeline in `models/sage16/modules/`, and what happens in the large-N limit. Written alongside the code review of `docs/dev/MIMIC-DYNAMICAL-TIMESTEP-PLAN.md` (commits `469b7adc..HEAD`).

---

## Summary

Most sage16 substep consumers are well-behaved rate-times-dt integrators (forward-Euler / operator-split) that converge, as intended, toward the correct continuum solution as `N = num_substeps → ∞`, subject to a real floating-point accuracy floor imposed by the single-precision (`float`) storage of every persistent gas/mass/metal reservoir. One module — `sage_satellite_stripping` — does **not** converge to a physically meaningful large-N limit at all: its formula converges to a fixed geometric asymptote (`1 − 1/e ≈ 63.2%` of the excess baryons stripped per snapshot interval) that is a pure artifact of the discretization and has no connection to `dT`, `Rvir`, `Vvir`, or any real stripping timescale.

**This is not a Mimic-introduced artifact.** Stock sage-model's `strip_from_satellite` (`sage-code/sage-model/model_infall.c:103`) divides the identical recomputed excess by the same fixed `STEPS` constant, called from inside the same per-`STEPS` substep loop (`core_build_model.c:354`) — Mimic's `sage_satellite_stripping` module faithfully reproduces this. Under fixed timestepping (stock SAGE always uses `STEPS=10`; sage16 ships `SubSteps: 10`) this artifact is invisible — `N` is constant everywhere, so it is absorbed into calibration, exactly as it always has been in every published SAGE result. Under dynamic timestepping, `N` now varies with redshift by design, so this pre-existing artifact becomes newly **visible as a redshift-dependent, physically spurious change in satellite-stripping efficiency** — full stripping at low z (where dynamic N collapses to 1) versus ~63% stripping at high z (where dynamic N saturates near `MAX_DYNAMIC_SUBSTEPS`). The correct response is to document this interaction, not to "fix" stripping — changing it would break sage16/sage-model parity and silently alter calibrated physics. See `models/sage16/modules/sage_satellite_stripping/sage_satellite_stripping.c` for the in-module comment and `test_stripped_fraction_follows_geometric_formula` (in that module's test file) for a regression test that pins down the documented N-dependence.

---

## How the substep loop is used, by pattern

Every module in the `galaxy_physics` / `satellite_mergers` phases (`models/sage16/input/sage16_mini-millennium.yaml`) runs once per substep, `N` times per snapshot interval. Reading the substep-count consumers grouped by mathematical structure:

### Pattern A — Fixed budget, evenly partitioned (converges exactly)

`sage_apply_infall.c:60`: `infallingGas = galaxy->InfallingGas / ctx->num_substeps`. `InfallingGas` is computed **once**, in `pre_timestep`, by `sage_prepare_infall_budget` — before any substep runs. Summing `N` equal slices of a value fixed before the loop starts reproduces exactly the same total regardless of `N` (up to floating-point rounding, see below). This is not an approximation of anything; it is just a partition. `N → ∞` changes nothing about the physical result, only the granularity at which it is deposited.

### Pattern B — Per-object rate × dt, current-state-dependent (converges to the analytic solution)

`sage_calculate_cooling_budget.c`, `sage_reincorporation.c`, `sage_radio_mode_heating.c`, `sage_calculate_star_formation.c`, `sage_apply_star_formation_supernova.c` all use `mimic_object_substep_dt()` (`models/sage16/shared/time_parity.h`) to get `dt = halo->dT / N`, then compute a rate from the **current** reservoir state and multiply by `dt`. These are forward-Euler discretizations of true decay/growth ODEs (e.g. cooling: `dHotGas/dt = -HotGas/t_cool`; reincorporation: `dEjectedGas/dt = -(Vvir/Vcrit - 1) * EjectedGas / t_dyn`). Iterating `N` such steps over the full interval converges, as `N → ∞`, to the correct exponential solution:

```
HotGas(dT) → HotGas(0) * exp(-dT / t_cool)      as N → ∞
```

This is textbook first-order convergence: global discretization error is `O(dT/N)`. More substeps means a **better** answer, up to the floating-point floor described below. This is the behavior the dynamic-timestep plan is implicitly relying on when it says dynamic mode should "integrate accumulating physics over many fractions of a dynamical time" at high z.

### Pattern C — Threshold/projection, re-evaluated each substep (insensitive to N by construction)

`sage_disk_instability.c` recomputes the Mo–Mao–White disk-stability criterion every substep and instantaneously moves any excess above `Mcrit` to the bulge. This is a constraint projection, not a rate process — it has no `dt` in it at all. Running it more often just catches instability sooner; it does not change the total amount transferred over an interval (whatever crosses the threshold gets moved either way). No N-dependent artifact here.

### Pattern D — Clock decrement + linear interpolation (converges, improves temporal resolution)

`sage_resolve_mergers_and_disruption.c`: `MergTime -= source_dt` (exact telescoping sum to `dT` regardless of `N`), and `current_mvir` is linearly interpolated across substeps via `(substep_number+1)/num_substeps`. Higher `N` gives finer-grained, more accurate timing of *when* within the interval a merger or disruption triggers relative to the halo's mass growth. This is a genuine benefit of higher N, with no artifact.

### Pattern E — Recomputed target, evenly partitioned by count, not by time (does *not* converge correctly)

`sage_satellite_stripping.c:76`:
```c
double strippedGas = -1.0 * (halo_baryon_frac * halo->Mvir - total_baryons) / (double)ctx->num_substeps;
```
Unlike Pattern A, `total_baryons` (hence the "excess" being stripped) is **recomputed from the satellite's current state on every substep call**, not fixed once at the start of the interval. Unlike Pattern B, the divisor is the literal substep count `N`, not a physical timescale multiplied by `dt` — `dT` does not appear anywhere in this formula. The result is a discrete process where each substep removes `1/N` of whatever excess remains *at that moment*:

```
excess_{k+1} = excess_k * (1 - 1/N)
```

so after all `N` substeps in the interval, the fraction of the original excess actually stripped is:

```
fraction_stripped(N) = 1 - (1 - 1/N)^N
```

This is independent of the snapshot's physical duration `dT` and has no connection to any tidal- or ram-pressure-stripping timescale. Concretely:

| N | fraction stripped |
|---|---|
| 1 | 100% (entire excess removed in one shot) |
| 2 | 75% |
| 5 | 67.2% |
| 10 | 65.1% |
| 20 | 64.2% |
| 50 (`MAX_DYNAMIC_SUBSTEPS`) | 63.6% |
| ∞ | 63.2% (`1 − 1/e`) |

The series converges fast — by `N=20` it is already within ~1 percentage point of the `N→∞` asymptote — so this is not a "resolve it better with more substeps" situation; it is a hard ceiling on what the formula can ever express, and the ceiling is a number invented by the discretization, not derived from physics.

**Why this was invisible until now:** `models/sage16/input/sage16_mini-millennium.yaml` fixes `SubSteps: 10` for every snapshot, so every satellite at every redshift is stripped at the same (arbitrary but constant) ~65.1% rate. That constant bias is indistinguishable from a calibration choice and gets absorbed into `GlobalBaryonFraction`/`HaloBaryonFraction` tuning.

**Why dynamic mode changes this:** dynamic mode ties `N` to `t_dyn(z)` by design (per `docs/dev/MIMIC-DYNAMICAL-TIMESTEP-PLAN.md`), so `N` genuinely varies from snapshot to snapshot: it collapses to 1 at low z (where `deltaT ≪ t_dyn`, per the plan's own "Key physical result") and grows toward `MAX_DYNAMIC_SUBSTEPS=50` at high z. Plugging that into the table above: switching a run from `fixed` to `dynamic` would, **as a pure side effect of the timestep scheme and with zero change to any stripping parameter**, push low-z satellite stripping from ~65% (fixed, N=10) toward ~100% (dynamic, N=1) and push high-z stripping from ~65% toward ~63.6% (dynamic, N≈50). The low-z shift is large and would show up directly in any statistic sensitive to satellite gas content or quenching (satellite red fraction, HI/gas-mass functions split by environment, etc.) — a user switching `TimestepScheme` expecting only "better temporal resolution" would silently get a materially different stripping physics instead.

---

## The floating-point accuracy floor (applies to all patterns)

Every persistent gas/mass/metal reservoir in `models/sage16/model_properties.yaml` — `HotGas`, `ColdGas`, `EjectedGas`, `StellarMass`, `BulgeMass`, `BlackHoleMass`, `ICS`, and all `Metals*` fields — is declared `type: float` (32-bit, ~7 significant decimal digits, relative machine epsilon ≈ 1.19e-7). The scratch/transport fields consumed *within* one substep (`InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, `CoolingLambda`) are deliberately kept `double`, per the `# SAGE parity: ... is a double local in SAGE` comments — this was already a conscious precision decision for the values that only exist transiently, but the *accumulators* they are eventually added into are not afforded the same protection.

Every substep across every Pattern-A/B module ends with a line like `galaxy->HotGas += infallingGas;` or `sat_gal->HotGas -= strippedGas;`, where the right-hand side shrinks roughly as `O(1/N)` while the left-hand side is a 24-bit-mantissa `float`. Once the per-substep increment falls below roughly half a ULP of the accumulator's current magnitude (i.e. `increment / |HotGas| ≲ 6e-8`), `HotGas += increment` becomes a silent no-op: the value is computed correctly in double precision by the compiler's implicit promotion, but the store back to the `float` field rounds it to the same bit pattern as before. Past that point, increasing `N` further does not refine the answer — it produces more roundoff/store operations while genuine physical mass increments are discarded rather than accumulated, so the discretization error (which is falling as `~1/N`) is eventually overtaken by roundoff error (which does not fall, and can even grow slightly with more operations). The total error curve in `N` is U-shaped, with a finite optimal `N*` rather than a monotonic improvement toward `N = ∞`.

- **Dynamic mode** cannot hit this in practice: `MAX_DYNAMIC_SUBSTEPS = 50` keeps `N` in a regime where per-substep increments for realistic galaxy masses (of order `0.01–100` in `1e10 Msun/h` units) are still comfortably above the float32 floor.
- **Fixed mode** has no such cap — `SubSteps` is a plain user-set YAML integer. A user chasing "more resolution" by setting `SubSteps: 100000` (reasonable-looking request, nothing in the code stops it) would, for low-mass satellites in particular, start silently losing mass to stagnation in `sage_apply_infall`, `sage_satellite_stripping`, and the cooling/reincorporation/AGN chains, with no warning or diagnostic — the run would complete normally and simply be scientifically wrong in a way that gets worse, not better, the higher `SubSteps` is set.

---

## Fixed vs. dynamic, side by side

| Aspect | Fixed (`SubSteps` literal) | Dynamic (`SubSteps` = resolution per `t_dyn`) |
|---|---|---|
| N range | User-set, unbounded above | `[1, MAX_DYNAMIC_SUBSTEPS=50]`, z-dependent |
| N vs. z | Constant across all snapshots | Small at low z (often 1), large at high z (up to 50) |
| Pattern A/B convergence | Correct, improves with N, until float32 floor | Correct, but capped at 50 — never reaches the asymptotic float32 concern |
| Pattern E (stripping) behavior | Constant, arbitrary bias baked into calibration (~65% at shipped `SubSteps=10`) | Varies by z: ~100% at low z → ~63.6% at high z — a new, physically unmotivated redshift trend |
| Practical "N → ∞" risk | Real: user can pick an N large enough to hit float32 stagnation | Not reachable: hard-capped, and the cap is already near the Pattern-E asymptote |
| Where the risk shows up | Any run where a user manually maximizes `SubSteps` for "better" resolution | Any comparison between fixed- and dynamic-mode runs of the same physics — the *scheme itself* changes stripping outcomes |

---

## Suggested improvements

Status markers below reflect what has actually been done as of this note's last update, not just proposed.

1. **Do not "fix" `sage_satellite_stripping`.** It matches stock SAGE exactly; changing the divide-by-`N` recompute pattern would break sage16/sage-model parity and silently alter calibrated physics. **Done:** documented in-module (`sage_satellite_stripping.c`) and pinned with a regression test (`test_stripped_fraction_follows_geometric_formula`).
2. **Add an N-invariance regression test** for every direct `/ctx->num_substeps` consumer: run the same physical interval at two or more different `N` values and assert the *total* quantity transferred over the full interval — invariant for Pattern A (`sage_apply_infall`), and matching the known geometric formula for Pattern E (`sage_satellite_stripping`) — not just that mass is conserved *within* a single N, which is what the pre-existing tests checked. **Done:** `test_physics_infall_budget_invariant_across_substep_counts` (infall) and `test_stripped_fraction_follows_geometric_formula` (stripping), both verified against the built module code, not just hand-derived.
3. **Consider promoting the persistent reservoirs most exposed to repeated small increments (`HotGas`, `ColdGas`, `EjectedGas` at minimum) to `double`,** or introduce compensated (Kahan) summation for the substep-accumulation call sites, if there is any intent to let `SubSteps` grow large in practice. **Decision:** sage16 keeps `float` reservoirs deliberately, for sage16/sage-model parity — documented via a comment at the top of `models/sage16/model_properties.yaml` and in `docs/DEVELOPER-GUIDE.md#property-precision`. New models without a parity constraint should default to `double` for accumulator-style properties (also now in the developer guide).
4. **`MAX_DYNAMIC_SUBSTEPS` sizing** — see the worked mini-Millennium numbers below. **Done:** exposed as an optional run-file parameter `MaxDynamicSubsteps` (default 200, `DEFAULT_MAX_DYNAMIC_SUBSTEPS` in `src/include/constants.h`) rather than a fixed compiled-in cap, so it can be tuned per simulation package. Documented in `docs/USER-GUIDE.md` and `docs/DEVELOPER-GUIDE.md#modulecontext-fields`.
5. Documenting the fixed-vs-dynamic stripping-efficiency difference in user-facing docs was considered and dropped in favor of the in-module/test documentation in items 1–2, since `docs/USER-GUIDE.md` is not the right place for a module-internal implementation detail.
6. **Widen core virial-mass-family properties to `double`** (`Mvir`, `deltaMvir`, `CentralMvir`, `Rvir`, `Vvir`, `infallMvir`, `infallVvir`, `infallVmax`) since core is shared by every model, not just sage16. **Done** — see the core/simulation precision section below; this surfaced and fixed a real historical-maximum-tracking bug in `inheritance.c`, requiring a baseline regeneration.
7. **Widen Consistent-Trees-sourced simulation catalog properties to `double`** (`micro-uchuu-hdf5`, `uchuu`, `micro-uchuu-ascii`). **Approved, not yet applied** — see the precision section below.

---

## How many steps actually run per snapshot? (mini-Millennium, empirical)

`MAX_DYNAMIC_SUBSTEPS` was assumed during the code review to cap the *user-facing* `SubSteps` value. It does not — `SubSteps` is reinterpreted in dynamic mode as *substeps per dynamical time*, and `MAX_DYNAMIC_SUBSTEPS` caps the *computed* `N = ceil(deltaT · SubSteps / t_dyn)` for each snapshot transition. Since `t_dyn` is mass-independent (`t_dyn(z) = 1/(10·H(z))`, confirmed in the code review), `N` for a given `SubSteps` value depends only on the snapshot list's `deltaT(z)/t_dyn(z)` ratio — computed here for mini-Millennium's 64-snapshot `mini-millennium.a_list` (`Ωm=0.25, ΩΛ=0.75`) by numerically reproducing `time_to_present()`'s integral from `src/core/init.c`. `H0` cancels exactly in the `deltaT/t_dyn` ratio, so this table needs no absolute unit normalization.

Unclamped `N` at representative snapshots, for several `SubSteps` (resolution-per-`t_dyn`) values:

| snap | z | SS=1 | SS=10 | SS=20 | SS=50 | SS=100 | SS=200 | SS=500 |
|---|---|---|---|---|---|---|---|---|
| 1 | 80.0 | 33735 | 337344 | 674687 | 1686716 | 3373431 | 6746861 | 16867153 |
| 4 | 19.9 | 1903 | 19026 | 38051 | 95127 | 190253 | 380506 | 951264 |
| 10 | 11.9 | 133 | 1321 | 2642 | 6603 | 13206 | 26412 | 66029 |
| 20 | 5.29 | 27 | 270 | 539 | 1346 | 2692 | 5384 | 13459 |
| 30 | 2.42 | 7 | 67 | 134 | 333 | 666 | 1332 | 3330 |
| 40 | 1.08 | 2 | 20 | 40 | 99 | 198 | 396 | 990 |
| 47 | 0.56 | 1 | 10 | 19 | 47 | 94 | 187 | 466 |
| 54 | 0.24 | 1 | 5 | 10 | 24 | 47 | 94 | 234 |
| 63 | 0.00 | 1 | 3 | 5 | 11 | 21 | 41 | 101 |

Two things fall out of this that materially change the earlier "large-N" analysis:

**The first few snapshots are not representative of "normal" behavior.** `deltaT/t_dyn` is ~33700 at snap 1 (z=80) and only drops below ~2000 by snap 4 (z≈20) — mini-Millennium samples the very early universe extremely coarsely (5 snapshots above z≈15), and `t_dyn` shrinks faster than `deltaT` as z grows, so the ratio blows up. No reasonable `MAX_DYNAMIC_SUBSTEPS` can "resolve" this regime; it would need a ceiling in the tens of thousands. This is fine in practice — essentially no baryonic physics is active in these tiny, just-formed halos (negligible `HotGas`), so under-resolving them costs nothing scientifically, but it does mean the clamp is *unconditionally* saturated there regardless of the chosen ceiling.

**With the current `MAX_DYNAMIC_SUBSTEPS=50`, the clamp dominates for most of cosmic history at the `SubSteps` values a user would actually pick.** For `SubSteps=50` (i.e. asking for 50-substep-per-`t_dyn` resolution), the unclamped `N` stays above 50 all the way from snap 1 through snap 46 (z≈0.62) — 73% of all 63 transitions — meaning the run behaves like fixed `N=50` for most of the simulated history and only becomes genuinely "dynamic" (uncapped, `SubSteps`-controlled) in the last ~17 snapshots. For `SubSteps=100`, the clamp dominates through snap 53 (z≈0.28, 84% of transitions). This matters directly for the empirical z=0-convergence test ("somewhere between `SubSteps=50` and `100`"): that convergence is driven by the low-z tail, where `N` is small and genuinely `SubSteps`-controlled (e.g. `N≈11` at z=0 for `SubSteps=50`, `N≈21` for `SubSteps=100`) — the clamp is not what's being tested there, but it *was* silently capping the mid-z contribution at exactly 50 for both cases, which may be part of why the two don't look more different.

**Recommendation.** `MAX_DYNAMIC_SUBSTEPS` should be treated purely as a safety ceiling against pathological blowup (sparse high-z snapshot spacing, config errors), not as something that determines physical resolution — the resolution is what `SubSteps` is for. It should be set generously relative to the largest `SubSteps` a user intends to explore for convergence testing (a few×, so the ceiling doesn't bind in the astrophysically active z≲2–3 regime where most stellar mass assembles), not tuned to "cover" the high-z tail, which is both scientifically unnecessary (negligible baryons there) and would require an impractically large constant (tens of thousands) to actually stop clamping. For mini-Millennium, given empirical convergence around `SubSteps≈50–100`, a ceiling around 200–300 would remove clamp interference through most of the astrophysically relevant range without being reckless. Because the "right" ceiling depends on the simulation's snapshot spacing (a 40-snapshot or 200-snapshot simulation will have different `deltaT/t_dyn` profiles), a single hardcoded constant in `constants.h` cannot be simulation-appropriate in general — **this is a real argument for exposing `MAX_DYNAMIC_SUBSTEPS` as an optional run-file parameter with a sensible default**, rather than a compiled-in constant, so it can be tuned per simulation package rather than requiring a maintainer/constants.h change each time. This is an open decision, not yet applied.

---

## Core and simulation property precision (beyond sage16's galaxy reservoirs)

- **`MimicConfig` foundational scalars** (`G`, `Hubble`, `Omega`, `OmegaLambda`, `PartMass`, `BoxSize`, `RhoCrit`, all `Unit*_in_*` conversions) are already `double` throughout `src/include/types.h`. No issues.
- **`src/core/core_properties.yaml`** (shared by every model): **Done.** `Mvir`, `deltaMvir`, `CentralMvir`, `Rvir`, `Vvir`, `infallMvir`, `infallVvir`, `infallVmax` were `float`, matching sage-model's `struct GALAXY` — a parity choice made at the **core** level, so it applied to non-SAGE models (SHAM, halos-only) with no parity reason to inherit it. Widening them to `double` surfaced a real, previously-silent bug: `apply_descendant_properties()` (`src/core/inheritance.c`) tracks each halo's historical-maximum `Rvir`/`Vvir` via `if (descendant->virial_mass > halo->Mvir)`, comparing a fresh `double` mass against the *previous* snapshot's stored mass. Under `float` storage that stored mass was rounded, so the comparison could go the wrong way for halos whose mass grew by an amount near the rounding boundary, silently freezing an orphan's preserved `Rvir`/`Vvir` at an earlier, smaller snapshot. With `double` storage the comparison is exact. This changed output for 73–91 of 9265 halos (~0.8–1%) in the mini-Millennium physics baseline (`tests/data/output/baseline/`), regenerated via `scripts/regenerate_baseline.sh` (HDF5) and the documented manual copy (binary) after confirming all 43 unit tests, all sampled integration tests, and the scientific virial-relation check still pass. Two hand-written call sites also needed updating to stop discarding the new precision: `output_helpers.h`'s `output_rvir_conditional`/`output_vvir_conditional`/`output_infall_property_or_zero` (previously hardcoded `float` return/parameter types with explicit casts) and a stray `float central_mvir` local in `build_model.c`.
- **Simulation catalog properties** (`simulations/<SIM>/halo_properties.yaml`): all declare `float` for masses/positions/velocities/spins uniformly across every package. For `lhalo_binary`-sourced simulations (`mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu`) this is correct and lossless — the on-disk format itself is single precision, so there is no extra precision to keep. For **Consistent-Trees-sourced simulations** (`micro-uchuu-hdf5`, `uchuu`: `tree_type: consistent_trees_hdf5`; `micro-uchuu-ascii`: `consistent_trees_ascii`), this is a real, avoidable precision loss: `src/io/tree/read_ctrees_hdf5.c` reads `Mvir`/positions/velocities/spins as native HDF5 `double` (`CT_ASSIGN_SINGLE(mvir, double, halos, Mvir)`), and the ASCII reader parses the same fields to `double` too — both then immediately store into the `float`-typed fields generated from `halo_properties.yaml`, discarding precision that genuinely exists in the source file, for no parity benefit. **Approved, not yet applied**: change `type: float` → `type: double` for the relevant fields in those three packages' `halo_properties.yaml`, regenerate, and check for baseline/regression data specific to those packages.
- Guidance for future model/simulation authors on all of the above is now in `docs/DEVELOPER-GUIDE.md#property-precision`: match `type:` to the real precision of the source (catalogs) or default to `double` for anything accumulated across substeps (galaxy properties), absent a specific parity constraint.

---

## Sketch: an N-invariant satellite-stripping alternative

**Status:** Sketch only, not a frozen plan. For a future implementation pass, not this one.

**Problem:** `sage_satellite_stripping.c` divides a *recomputed* excess by `num_substeps` every substep, so the fraction of excess baryons stripped over one snapshot interval is `1-(1-1/N)^N` — 100% at N=1, converging to ~63% as N grows, with no connection to `dT` or a physical timescale (see "Pattern E" above). This is inherited, byte-identical stock-SAGE behavior, so it must not change for sage16's default `TimestepScheme: fixed` path. Under `TimestepScheme: dynamic`, it produces a redshift-dependent stripping-efficiency artifact purely from the timestep scheme.

**Do not edit `sage_satellite_stripping.c` in place.** Any change to its formula breaks sage16↔sage-model parity for the shipped `SubSteps: 10` fixed-mode default. Ship a new, separate module instead, and let users opt in via the run YAML's module phase list — same pattern as any alternate physics prescription in Mimic (see `docs/VISION.md` principle 2, runtime modularity).

**Two candidate formulas for the new module** (pick one; both remove the N-dependence):

1. **Fixed-budget partition** (mirrors `sage_apply_infall`/`sage_prepare_infall_budget`): compute the baryon-fraction excess *once* per snapshot interval in `pre_timestep` (a new `sage_prepare_stripping_budget`-style module, or fold into the existing prepare-infall-budget module), store it in a transport property, then divide by `num_substeps` and apply evenly each substep — same shape as `sage_apply_infall.c:60`. Simple, no new physical parameter, but the total stripped per interval is still whatever the instantaneous excess was at the start of the interval, not a smoothly time-resolved process.
2. **Physical stripping timescale** (mirrors `sage_reincorporation`/`sage_calculate_cooling_budget`): give stripping a genuine timescale `t_strip` (e.g. reuse `t_dyn = Rvir/Vvir` of the *central*, or a ram-pressure-motivated scaling), and apply `rate * dt` via `mimic_object_substep_dt()` each substep, so the fraction stripped over a full interval is `1 - exp(-dT/t_strip)` — a real, N-independent-in-the-limit physical quantity that also resolves faster/slower stripping with more/fewer substeps like cooling does. More physically motivated, but introduces a new modeling choice (which timescale, what normalization) that needs its own calibration/validation against observations or the literature, not just a numerical fix.

**Suggested shape of the work**, for whichever formula is chosen:
- New module directory `models/sage16/modules/sage_satellite_stripping_v2/` (or similar name), with `module_info.yaml`, following the `mimic-modules` skill's directory-module pattern.
- Reuse `mimic_get_metallicity` and the symmetric-metal-transfer fix already present in the original module (that part is a genuine improvement over stock SAGE and should carry over).
- Add an N-invariance test analogous to `test_physics_infall_budget_invariant_across_substep_counts`: run the same physical interval at two different `N` and assert the total stripped mass matches (formula 1) or matches `1-exp(-dT/t_strip)` within tolerance (formula 2) — not just conservation within one N.
- Document in the module README that this is an alternative to `sage_satellite_stripping`, why it exists (N-invariance for `TimestepScheme: dynamic`), and that it is not SAGE-parity-preserving by construction.
- Leave the original `sage_satellite_stripping` and its test untouched; sage16's default run YAML keeps using it.
