# `sage_positive_agn_feedback`

High-redshift **positive** AGN feedback for Mimic's SAGE pathway, following

> Silk, Begelman, Norman, Nusser & Wyse (2024),
> *"Which came first: supermassive black holes or galaxies? Insights from JWST"*,
> [arXiv:2401.02482](https://arxiv.org/abs/2401.02482).

## Purpose

Mimic already models the *negative* side of AGN feedback (cooling suppression in
`sage_radio_mode_heating`, energy-driven winds in `sage_quasar_mode`). This
module adds the early, *positive* side that the paper argues dominated at high
redshift. From the Abstract (verbatim):

> "AGN feedback evolved from a short-lived, high redshift phase in which
> radiatively cooled turbulence and/or momentum-conserving outflows stimulated
> vigorous early star formation ('positive' feedback), to late,
> energy-conserving outflows that depleted halo gas reservoirs and quenched star
> formation. The transition between these two regimes occurred at z∼6,
> independently of galaxy mass, for simple assumptions about the outflows and
> star formation process."

So at high `z` the AGN outflow shock-compresses the dense ISM, the shocked gas
cools, and the cold dense phase forms stars **vigorously**; at low `z` the same
AGN quenches (handled by the other modules).

## Physics

The regime is decided by a column-density criterion:

- Host ISM column (paper §3.3): `N_H = 10^21 (1+z)^3.3 cm^-2` — rises steeply
  toward high `z`, and carries **no galaxy-mass dependence**.
- Transition column (paper §5.1): `N_cool ≈ 10^23 cm^-2 (v_s/3000 km s^-1)^2` —
  the minimum obscuration for the momentum→energy (positive→negative) switch,
  exposed here as the parameter `PositiveFeedbackColumnThreshold`.

`N_H > N_cool` ⇒ momentum-conserving ⇒ **positive**. Because `N_H(z)` is
mass-independent and the threshold is set by outflow physics, the crossover is
near `z ~ 6` for all masses — the paper's headline result. We turn this into a
smooth weight `f_pos(z) = N_H / (N_H + N_cool) ∈ [0,1]` (½ at the crossover,
→1 at high `z`, →0 at low `z`).

The triggered star-formation rate is an `f_pos`-weighted enhancement of the disc
SF law (mirroring `sage_calculate_star_formation`):

```
SFR_trig = PositiveFeedbackEfficiency * f_pos * ColdGas / t_dyn,   t_dyn = DiskScaleRadius / Vvir
```

with `v_s ≈ Vvir` (paper §2: "shocks at close to the virial speed,
`v_s ≈ 600 (M10/r150)^1/2 km s^-1`"). **Modelling note:** the paper is analytic
and does not provide a closed-form SFR-boost for a SAM; this rate is a
deliberate, paper-grounded choice for "vigorous early star formation", and is
clearly flagged as such in the source.

## Processing contract

- Supported mode: `process_by_galaxy`
- Pipeline position: `phase_1`, **after** `sage_calculate_star_formation` and
  **before** `sage_apply_star_formation_supernova`.
- Reads: `Vvir`, `ColdGas`, `DiskScaleRadius`, `BlackHoleMass`, `dT`.
- Writes: adds triggered stars to the `NewStellarMass` transport field (committed
  — recycling, metals, `StarFormationRate` — by the apply step), and accumulates
  the diagnostic output `AGNTriggeredStellarMass`.
- Gating: only fires when an AGN exists (`BlackHoleMass > 0`) and there is cold
  gas, a finite disc, and `f_pos > 0`.

## Parameters

| Parameter | Meaning | Suggested |
| --- | --- | --- |
| `PositiveFeedbackEfficiency` | Strength of triggered SF (`[0,10]`) | `0.05` |
| `PositiveFeedbackColumnThreshold` | `N_cool` in cm⁻², sets the transition redshift (`N_H=N_cool` at `z~6`) | `6.0e23` |

## Tests

Unit tests in `_tests/test_unit_sage_positive_agn_feedback.c` cover the high-z
trigger, the low-z negative regime, the no-black-hole gate, additive
`NewStellarMass` behaviour, edge cases, and efficiency sensitivity.
