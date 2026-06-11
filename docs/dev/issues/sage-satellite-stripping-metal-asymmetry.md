# Satellite stripping can destroy hot-gas metals (inherited SAGE bug, adopted for parity)

**Status**: ACCEPTED — bug adopted deliberately to preserve byte-identical parity with published SAGE. Do not fix without reading "Remediation path" below.
**Date**: 2026-06-11 (review finding B3 of `docs/MODELS-SIMULATIONS-REVIEW.md`; verified against SAGE source the same day)
**Severity**: Low in practice (requires an already-unphysical reservoir state to trigger; see "When it actually fires"), but it is a genuine conservation violation and a trap for future refactoring.
**Affected code**: `models/sage16/modules/sage_satellite_stripping/sage_satellite_stripping.c` (`sage_satellite_stripping_process`, stripping transfer block)
**Upstream origin**: `sage-code/sage-model/model_infall.c:106-118` (`strip_from_satellite`), with `get_metallicity` from `sage-code/sage-model/model_misc.c:107-117`

---

## Summary

When a Type 1 satellite's baryon content exceeds its reionization-modified baryon budget, the excess hot gas is stripped to the FOF central over the substeps. The metal bookkeeping in this transfer computes the satellite's metal *loss* and the central's metal *gain* through two different expressions that are equal in the common case but diverge when the hot-gas metallicity is at its cap (`MetalsHotGas >= HotGas`, so `Z` is clamped to 1). In that regime, metals are destroyed: the satellite loses more metals than the central receives. The same code, line for line, exists in the released SAGE model that Mimic's byte-identical baseline is built against, so Mimic reproduces it on purpose.

## The code

Mimic (structure identical to SAGE's `strip_from_satellite`):

```c
const double metallicity = mimic_get_metallicity(sat_gal->HotGas, sat_gal->MetalsHotGas);
double strippedMetals = strippedGas * metallicity;          /* from UNCLAMPED gas   */

if (strippedGas > sat_gal->HotGas)                          /* clamps applied       */
  strippedGas = sat_gal->HotGas;                            /*   independently      */
if (strippedMetals > sat_gal->MetalsHotGas)
  strippedMetals = sat_gal->MetalsHotGas;

sat_gal->HotGas -= strippedGas;
sat_gal->MetalsHotGas -= strippedMetals;                    /* satellite loses m'   */
cen_gal->HotGas += strippedGas;
cen_gal->MetalsHotGas += strippedGas * metallicity;         /* central gains s'·Z   */
```

`mimic_get_metallicity` (= SAGE `get_metallicity`) returns `Z = metals/gas` when both are positive, **capped at 1.0**, else 0.

Two structural oddities, both inherited from SAGE: (1) `strippedMetals` is computed from the *unclamped* gas demand and then clamped independently of the gas; (2) the central is credited with the recomputed product `strippedGas * metallicity` rather than with the `strippedMetals` actually debited from the satellite.

## When it actually fires (the precise algebra)

Let `G = HotGas`, `M = MetalsHotGas`, `Z = mimic_get_metallicity(G, M)`, `s` = unclamped stripping demand (> 0). Satellite metal loss is `min(s·Z, M)`; central metal gain is `min(s, G)·Z`.

| Regime | Condition | Satellite loses | Central gains | Conserved? |
|---|---|---|---|---|
| No clamp | `s ≤ G`, `Z = M/G` | `s·Z` | `s·Z` | yes |
| Gas clamp, normal Z | `s > G`, `M < G` (so `Z = M/G`) | `min(s·M/G, M) = M` | `G·(M/G) = M` | yes |
| Gas clamp, capped Z, both clamps | `s > M ≥ G` (so `Z = 1`) | `M` | `G` | **no — destroys `M − G`** |
| Gas clamp, capped Z, gas clamp only | `M ≥ s > G` (so `Z = 1`) | `s` | `G` | **no — destroys `s − G`** |

The key point the original review finding (B3) stated too loosely: in *exact* arithmetic with `Z = M/G`, the two clamps engage together and the two expressions agree — the transfer is conservative even when clamped. **Metal destruction occurs exactly when the metallicity cap is active (`M ≥ G`) and the gas clamp engages (`s > G`); the destroyed mass is `min(s, M) − G`.** In the "gas clamp only" row, the satellite is additionally left in a stranded state (`HotGas = 0` with `MetalsHotGas > 0`), which `get_metallicity` subsequently treats as `Z = 0` until a merger or disruption moves the remaining metals.

A second-order effect exists in all regimes: reservoirs are `float` while `Z` and the products are `double`, so marginal cases near the clamp boundaries can mismatch at the ~1 ulp level. This is ordinary float accumulation noise, not the bug.

`M ≥ G` is an unphysical state (metal mass exceeding the gas that carries it), but it is reachable in practice through float cancellation and through transfer paths that move gas and metals on different expressions — which is exactly why `mimic_get_metallicity` documents "negative metal reservoirs (possible from float cancellation)" and caps `Z` at all.

## Why we keep it

Mimic enforces output parity with the released SAGE code: the full-physics baseline (`models/sage16/modules/_tests/baseline/`, gated by `test_scientific_sage_physics_baseline.py`) is byte-identical to `sage-model` output, and that byte-identity is the project's strongest regression instrument (see `docs/MIMIC-SAGE-PARITY-ANALYSIS` history and the dual-driver byte gates). The stripping metal bookkeeping is not specified at this level of detail in the published model papers (Croton et al. 2006, MNRAS 365, 11; Croton et al. 2016, ApJS 222, 22) — the paper defines *what* is stripped (the excess over the reionization-modified baryon budget, spread over substeps), not the clamp ordering. Parity therefore means matching the *code* that produced the published results, including this slip. Fixing it unilaterally would silently break the byte gate and decouple Mimic from the reference implementation.

The adoption is marked at the code site with a parity comment warning against "fixing" it casually.

## The correct implementation (when parity is relaxed)

The intent is plainly a symmetric transfer at the satellite's hot-gas metallicity. Clamp the gas first, derive the metals from the clamped gas, clamp once against availability, and move the *same* quantity out of the satellite and into the central:

```c
const double metallicity = mimic_get_metallicity(sat_gal->HotGas, sat_gal->MetalsHotGas);

if (strippedGas > sat_gal->HotGas)
  strippedGas = sat_gal->HotGas;

double strippedMetals = strippedGas * metallicity;
if (strippedMetals > sat_gal->MetalsHotGas)
  strippedMetals = sat_gal->MetalsHotGas;

sat_gal->HotGas -= strippedGas;
sat_gal->MetalsHotGas -= strippedMetals;
cen_gal->HotGas += strippedGas;
cen_gal->MetalsHotGas += strippedMetals;     /* same quantity both sides */
```

This conserves metals identically in every regime, including `Z = 1`, and leaves no stranded-metals state beyond what the satellite legitimately retains. (A stricter variant would also drain the satellite's remaining metals when `HotGas` reaches exactly 0, but that is a modelling choice, not part of the bug fix.)

## Remediation path

1. **Decision first**: fixing this is a deliberate, documented divergence from the published SAGE implementation. It needs an explicit project decision (and ideally a changelog entry stating Mimic > SAGE fidelity on this point).
2. **Quantify before changing**: instrument the `Z = 1` + gas-clamp branch with a counter (or one-off `WARNING_LOG`) and run the mini-Millennium and Millennium-subset configurations to measure how often it fires and how much metal mass is destroyed. If it never fires on the reference volumes, the fix is output-neutral on those volumes and the baseline may not even change — verify rather than assume.
3. **Apply the fix** above in `sage_satellite_stripping.c`; add a unit test asserting satellite-loss == central-gain in all four regimes of the table (including a constructed `M > G` state).
4. **Regenerate the physics baseline** with `scripts/regenerate_baseline.sh` if outputs change, and record the divergence in the baseline provenance and in `docs/MIMIC-SAGE-PARITY-ANALYSIS` successor notes — from that point on, byte-comparison against stock `sage-model` is expected to differ in exactly this channel.
5. Update the parity comment at the code site and mark this issue RESOLVED with the commit hash.

## History

- 2026-06-11: flagged as finding **B3** in `docs/MODELS-SIMULATIONS-REVIEW.md` during the models/simulations code-simplifier review; initially described (too broadly) as firing "when a clamp engages".
- 2026-06-11: verified line-for-line identical in `sage-code/sage-model/model_infall.c:106-118`; classified as SAGE parity and adopted (commit `51bc96f`); precise trigger condition derived (this document) and the code comment tightened accordingly.
- User decision (2026-06-11): treat as a genuine bug in original SAGE, adopted knowingly under the parity policy; this report records the full context and the fix for future review.
