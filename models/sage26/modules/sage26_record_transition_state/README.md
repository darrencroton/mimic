# `sage26_record_transition_state`

Records each galaxy's final `Type` at the end of a snapshot, for SAGE26's progenitor-joining transitions at the next one, and writes the outputs `infallStellarMass` and `TimeOfInfall` by SAGE26's output rule. Mimic's core assigns a galaxy's new `Type` when it copies the galaxy into its descendant but does not keep the old one; this module records it so that [`sage26_initialise_merger_clock`](../sage26_initialise_merger_clock/README.md) can decide each galaxy's merger clock and infall values from the `Type` it ended the previous snapshot with.

For every galaxy row of the FoF (rows merged this snapshot included):

```text
PreviousType      = Type
infallStellarMass = Type != 0 ? infallStellarMassState : 0
TimeOfInfall      = Type != 0 ? TimeOfInfallState      : 0
```

`PreviousType` is inherited, so the galaxy carries it into the next snapshot, where `sage26_initialise_merger_clock` reads it in `pre_timestep` (as does `sage26_set_halo_concentration` with `ConcentrationOn 3`); a galaxy the core creates has `PreviousType` -1. `infallStellarMassState` and `TimeOfInfallState` keep their values for a central (so a central that later becomes a satellite off the main branch keeps an earlier `TimeOfInfall`); only the written outputs are 0 for a Type 0 row. The core writes `infallMvir`, `infallVvir` and `infallVmax` by the same rule.

## Processing Contract

- Supported mode: `process_full_halo`, in `post_timestep`, exactly once, as the last entry. `init()` refuses an entry in `pre_timestep` or a substep phase, a second entry, and any `post_timestep` module after it: a later module could still change a `Type`, and an earlier placement would record a `Type` the merger pass has not finished changing.
- Receives the full FoF workspace

## Ordering

- This module's `init()` does not require `sage26_initialise_merger_clock`, the only writer of `infallStellarMassState` and `TimeOfInfallState`; without it, a row that is not a Type 0 is written with their creation value -1.
- In the other direction, `sage26_initialise_merger_clock`'s `init()` warns (and the run continues) when this module is absent from `post_timestep`, and `sage26_set_halo_concentration`'s `init()` refuses its absence with `ConcentrationOn 3`.

## Properties

- Reads: `Type`, `infallStellarMassState`, `TimeOfInfallState`
- Writes: `PreviousType`, `infallStellarMass`, `TimeOfInfall`

## Parameters

None.

## Notes

The output rule is applied once per snapshot to every galaxy row, rather than at write time as in SAGE26; a row this module has not processed keeps the creation value -1.

## Tests

- `_tests/test_unit_sage26_record_transition_state.c`: the recorded `Type` and output rule, and every joining transition across two snapshots with `sage26_initialise_merger_clock`.
- `_tests/test_integration_sage26_record_transition_state.py`: the infall values of galaxies followed across consecutive snapshots of full runs, with and without the merger phase.
- The placement checks are covered at package level by `../_tests/test_unit_sage_dependency_contracts.c`.
