# `sage26_initialise_merger_clock`

Makes SAGE26's merger-clock, infall and history writes of progenitor joining, once per FoF group at the start of each snapshot. Mimic's core assigns each galaxy's new `Type`, its halo values and, for a Type 0 that becomes a satellite, `infallMvir`, `infallVvir` and `infallVmax`; this module reads the galaxy's `PreviousType` (recorded by `sage26_record_transition_state` at the end of the previous snapshot) and makes the remaining writes: it zeroes the SFR histories of a galaxy that stays a Type 0 or 1, resets every central's clock, records `infallStellarMass` and `TimeOfInfall` when a central becomes a satellite, estimates a new satellite's dynamical-friction clock, and starts the clock of a galaxy that loses its subhalo at zero.

## Physics

The transitions follow SAGE26's `join_galaxies_of_progenitors`:

- Type 0 or 1 that stays a Type 0 or 1: the sixty SFR history elements (`shared/sage_sfr_history.h`) are zeroed, before any other write. This is the histories' only reset after creation, so an orphan keeps its histories and carries them into a merger.
- Type 0 or 1 that becomes a Type 0: `MergTime = 999.9`.
- Type 0 that becomes a Type 1: `infallStellarMass = StellarMass` and `TimeOfInfall` = the snapshot number, as a float.
- Type 0, or Type 1 with `MergTime > 999`, that becomes a Type 1: `MergTime` is estimated (below).
- Type 0, or Type 1 with `MergTime > 999`, that becomes a Type 2: `MergTime = 0` and `infallStellarMass` is recorded; `TimeOfInfall` and the histories are kept.
- Type 1 with `MergTime <= 999` keeps its clock and infall values; a Type 2 is left unchanged; a galaxy the core created this snapshot (`PreviousType` -1) keeps its creation clock 999.9.

The clock is SAGE26's `estimate_merging_time`, the Binney & Tremaine (1987) dynamical-friction time, computed by `mimic_sage_estimate_merging_time` (`shared/sage_merger_clock.h`): `MergTime = MergerTimeFactor * 1.17 * Rvir^2 * Vvir / (ln(1 + Len_central / Len_sat) * G * M_sat)`, where `Rvir` and `Vvir` belong to the FoF's Type 0 row (computed from its `Mvir` at the snapshot's redshift, not the frozen stored `Rvir`/`Vvir`) and `M_sat = Mvir + StellarMass + ColdGas` of the satellite. The result is -1 when `M_sat` or the Coulomb logarithm is not positive or the satellite has fewer than 10 particles, and is capped at 998.

`infallStellarMassState` and `TimeOfInfallState` hold the values for every row; `sage26_record_transition_state` writes the output properties `infallStellarMass` and `TimeOfInfall` from them, storing 0 for a Type 0 row as SAGE26's writer does.

## Processing Contract

- Supported mode: `process_full_halo`, in `pre_timestep` only, exactly once. `init()` refuses an entry in a substep phase or `post_timestep` (a later entry would re-estimate a clock the merger pass had started decrementing) and anything but exactly one `pre_timestep` entry as `process_full_halo`.
- Receives the full FoF workspace: a satellite's clock reads the FoF's Type 0 row; a satellite with no Type 0 row in its FoF is an error.

## Ordering

- Must precede `sage26_resolve_mergers_and_disruption`, whose `init()` refuses a run without this module (satellites would keep the unset clock 999.9) and checks the order when both share `pre_timestep`. `sage26_finalize_sfr_histories`'s `init()` also refuses a run without it, since it owns the history reset.
- Should precede every `pre_timestep` module that changes `StellarMass` or `ColdGas` or touches a history; this is not checked. The shipped run files place it first in `pre_timestep`.
- Without `sage26_record_transition_state` in `post_timestep` as `process_full_halo`, `init()` warns and the run proceeds: clocks are still assigned, but the infall state is not recorded and `infallStellarMass` and `TimeOfInfall` are written as -1 on every row.

## Properties

- Reads: `HaloNr`, `Type`, `Len`, `Mvir`, `StellarMass`, `ColdGas`, `PreviousType`, `MergTime`
- Writes: `MergTime`, `infallStellarMassState`, `TimeOfInfallState`, the sixty history elements `<history>Bin<k>`

## Parameters

- `MergerTimeFactor` (optional): the clock's scaling factor; absent, it is SAGE26's default 2.0, which the shipped run files also set. `init()` accepts a value in (0, 1000].

## Tests

- `_tests/test_unit_sage26_initialise_merger_clock.c`: the placement checks, `MergerTimeFactor`, and each joining transition on hand-built workspaces.
- `_tests/test_integration_sage26_initialise_merger_clock.py`: the module in a full run, its placement refusals and the parameter.
