# `sage26_record_transition_state`

Records each galaxy's final `Type` at the end of a snapshot, for SAGE26's progenitor-joining transitions at the next one, and writes the outputs `infallStellarMass` and `TimeOfInfall` by SAGE26's output rule.

## SAGE26 Source

SAGE26's progenitor joining decides a galaxy's merger clock and infall values from its `Type` before joining, the `Type` it ended the previous snapshot with (`src/core_build_model.c:240-341` in the staged SAGE26 source; the table is in `sage26_initialise_merger_clock/README.md`). Its HDF5 writer stores the infall values for a galaxy that is not a central and 0 for a central (`src/io/save_gals_hdf5.c:1004-1017`):

```c
if(g->Type != 0) {
    infallMvir = g->infallMvir;  /* ... infallVvir, infallVmax */
    infallStellarMass = g->infallStellarMass;
    TimeOfInfall = g->TimeOfInfall;
} else {
    /* all five 0.0 */
}
```

The members themselves keep their values: a central that later loses its subhalo off the main branch keeps an earlier `TimeOfInfall` (`src/core_build_model.c:327-335`).

## What This Module Does

Mimic's core assigns the new `Type` while it copies a galaxy into its descendant (`src/core/inheritance.c`) and does not publish the old one. This module, the last `post_timestep` module, runs once per FoF group after the last substep and, for every galaxy row (rows merged this snapshot included; rows without a galaxy skipped):

```text
PreviousType      = Type
infallStellarMass = Type != 0 ? infallStellarMassState : 0
TimeOfInfall      = Type != 0 ? TimeOfInfallState      : 0
```

`PreviousType` is inherited (no `init_repeat`), so the galaxy carries it into the next snapshot, where `sage26_initialise_merger_clock` reads it in `pre_timestep` before any module changes it. A galaxy the core creates has `PreviousType` -1 (no previous snapshot). `infallStellarMassState` and `TimeOfInfallState` are the package-private copies of SAGE26's members, which `sage26_initialise_merger_clock` sets at joining; this module never changes them. The core writes `infallMvir`, `infallVvir` and `infallVmax` with the same rule through its own output function (`output_infall_property_or_zero`, `src/module_system/output_helpers.h:53-55`).

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `post_timestep`, exactly once, as the last entry. `init()` rejects an entry in `pre_timestep` or a substep phase, a second entry, and any `post_timestep` module after it: a later module could still change a `Type`, and an earlier placement would record a `Type` the merger pass has not finished changing
- Receives the full FoF workspace

## Properties

- Reads: `Type`, `infallStellarMassState`, `TimeOfInfallState`
- Writes: `PreviousType`, `infallStellarMass`, `TimeOfInfall`

## Parameters

None.

## Differences From SAGE26

- SAGE26 needs no such module: its joining reads the old `Type` before writing the new one. Publishing the previous `Type` from the core would make it unnecessary (a core-boundary item).
- SAGE26 applies the output rule when it writes a galaxy; this module applies it once per snapshot to every galaxy row, so a row written at this snapshot carries it. A row this module has not processed keeps the creation value -1.

## Tests

- `_tests/test_unit_sage26_record_transition_state.c` covers the placement check, the recorded `Type` on every row and the output rule, and runs two snapshots through the core's inheritance service with `sage26_initialise_merger_clock`, checking each transition case (0 to 1, 1 to 1, 1 to 0, non-main 0 to 2, 1 to 2, 2 to 2 and a new galaxy) and the written values.
- `_tests/test_integration_sage26_record_transition_state.py` runs the package pipeline over snapshots 30 to 63, follows galaxies between consecutive snapshots, and checks each written transition (0 to 0, 0 to 1, 1 to 1, 1 to 0) and that every Type 0 row is written with the five infall values 0. Orphans are made and consumed within one snapshot, so no Type 2 row is written; the unit test covers their transitions.
