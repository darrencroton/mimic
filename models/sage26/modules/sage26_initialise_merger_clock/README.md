# `sage26_initialise_merger_clock`

Makes SAGE26's merger-clock, infall and history writes of progenitor joining, once per FoF group at the start of each snapshot: it zeroes the SFR histories of a galaxy that stays a Type 0 or 1, resets every central's clock, records `infallStellarMass` and `TimeOfInfall` when a central becomes a satellite, estimates a new satellite's dynamical-friction clock with SAGE26's `estimate_merging_time`, and starts the clock of a galaxy that loses its subhalo at zero.

## SAGE26 Source

SAGE26 joins a halo's progenitor galaxies in `join_galaxies_of_progenitors` (`src/core_build_model.c:240-341` in the staged SAGE26 source). For a galaxy that was a Type 0 or 1 at the end of the previous snapshot it reads that Type before assigning the new one:

| Previous Type | New Type | SAGE26 writes | Lines |
|---|---|---|---|
| 0 or 1 | 0 or 1 (the main branch) | the six SFR histories zeroed, every bin | `:252`, `:278-282` |
| 0 or 1 | 0 (main branch of a FoF's first halo) | `MergTime = 999.9f` | `:284-300` |
| 0 | 1 (main branch of a subhalo) | `infallMvir`, `infallVvir`, `infallVmax` (previous values), `infallStellarMass = StellarMass`, `TimeOfInfall` = the descendant halo's snapshot number, as a float | `:306-313` |
| 0, or 1 with `MergTime > 999` | 1 | `MergTime = estimate_merging_time(halo, FoF's first halo)` | `:315-318` |
| 0, or 1 with `MergTime > 999` | 2 (not the main branch) | `MergTime = 0`, the three halo infall values and `infallStellarMass`; not `TimeOfInfall`; the histories are kept | `:322-337` |
| 1 with `MergTime <= 999` | 1 or 2 | the clock and the infall values are kept | |
| 2 | 2 | nothing (a Type 2 is copied unchanged) | `:240` |

`estimate_merging_time` (`src/model_mergers.c:89-121`):

```c
if(sat_halo == mother_halo) return -1.0;
const double coulomb = log1p(halos[mother_halo].Len / ((double) halos[sat_halo].Len));
const double SatelliteMass = get_virial_mass(sat_halo, ...) + StellarMass + ColdGas;
const double SatelliteRadius = get_virial_radius(mother_halo, ...);
if(SatelliteMass > 0.0 && coulomb > 0.0 && halos[sat_halo].Len >= 10)
    mergtime = MergerTimeFactor * 1.17 * SatelliteRadius * SatelliteRadius
               * get_virial_velocity(mother_halo, ...) / (coulomb * G * SatelliteMass);
else
    mergtime = -1.0;
if(mergtime >= 999.0) mergtime = 998.0;
```

## What This Module Does

Mimic's core performs the joining itself (`src/core/inheritance.c`): it assigns the new `Type`, the halo values and, for a Type 0 that becomes a Type 1 or 2, the core properties `infallMvir`, `infallVvir` and `infallVmax`. It does not publish the previous Type, so the package records it: `sage26_record_transition_state`, the last `post_timestep` module, writes each galaxy's final `Type` into the package property `PreviousType`, which the galaxy carries into the next snapshot. In `pre_timestep` this module reads it and makes the remaining writes of the table for every galaxy row of the FoF:

- `MergTime`, as the table says;
- `infallStellarMassState` and `TimeOfInfallState`, the package-private properties that hold SAGE26's members `infallStellarMass` and `TimeOfInfall`. SAGE26 keeps the members for a central and only its writer stores 0 for a Type 0 row (`src/io/save_gals_hdf5.c:1004-1017`); `sage26_record_transition_state` writes the output properties from the state with that rule;
- the sixty history elements (`shared/sage_sfr_history.h`), zeroed by `mimic_sage_reset_sfr_histories` for a galaxy that stays a Type 0 or 1, before its other writes, as SAGE26 does at `:278-282`. The histories are inherited package properties with no `init_repeat`, so this is their only reset after creation: a galaxy that becomes an orphan keeps its previous histories and, if it merges in the same snapshot, `add_galaxies_together` moves them into its target's bulge histories (`src/model_mergers.c:529-532`). The reset depends on the new `Type` alone (a galaxy that was a Type 2 is never a Type 0 or 1 again), so it is exactly SAGE26's set with or without the recorder.

The clock is `mimic_sage_estimate_merging_time` (`shared/sage_merger_clock.h`), SAGE26's expression in its operand order. The core does not keep SAGE26's raw halo catalogue, so the halo helpers' values come from what the core exposes for the current snapshot: the satellite's `Mvir` row value is `get_virial_mass` of its halo (the core sets it at joining with SAGE26's rule), and the mother halo, the FoF's first halo, is the FoF's Type 0 row, whose virial radius and velocity are the core's `virial_radius_for_mass` and `virial_velocity_for` of its `Mvir` at the snapshot's redshift. The galaxies' stored `Rvir` and `Vvir` are not used: the core keeps them frozen while a halo loses mass.

A galaxy the core created this snapshot has no previous Type (`PreviousType` -1) and keeps its creation clock 999.9f. The clock half of each condition reads `MergTime` alone, so without `sage26_record_transition_state` clocks are still assigned, but the infall state is not recorded, and `infallStellarMass` and `TimeOfInfall` are written as their creation value -1 on every row; `init()` warns in that case.

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `pre_timestep` only, exactly once. `init()` rejects an entry in a substep phase or `post_timestep`: the joining writes happen once per snapshot, and a later entry would re-estimate a new satellite's clock after the merger pass had started decrementing it. It also rejects anything but exactly one `pre_timestep` entry as `process_full_halo` (no entry, two entries, or one in another mode); each refusal names the module
- Receives the full FoF workspace: a satellite's clock reads the FoF's Type 0 row

## Properties

- Reads: `HaloNr`, `Type`, `Len`, `Mvir`, `StellarMass`, `ColdGas`, `PreviousType`, `MergTime` (the unset-clock tests)
- Writes: `MergTime`, `infallStellarMassState`, `TimeOfInfallState`, the sixty history elements `<history>Bin<k>`

## Parameters

- `MergerTimeFactor` (optional): the clock's scaling. Absent, it is SAGE26's default 2.0 (`src/core_read_parameter_file.c:128`), which R0 runs with; the shipped run files set 2.0. `init()` accepts a value in (0, 1000] (`LOAD_AND_VALIDATE_RANGE_EXCLUSIVE`, which rejects `value <= 0` and `value > 1000`; a NaN fails neither comparison and is accepted). SAGE26 does not range-check it (`src/core_read_parameter_file.c:206`); the check is the package's parameter convention.

## Ordering

1. Runs in `pre_timestep` before `sage26_resolve_mergers_and_disruption`, which hard-errors at `init()` without it (satellites would keep the unset clock 999.9) and checks the order when both share `pre_timestep`.
2. Runs before any `pre_timestep` module that changes `StellarMass` or `ColdGas`: SAGE26's joining, which reads them for `infallStellarMass` and the clock's satellite mass, precedes all physics of the snapshot. The shipped run files place it first in `pre_timestep`, before `sage26_set_disk_scale_radius` (the two touch disjoint state: swapping them leaves every galaxy dataset of the output identical).
3. Runs before any module reads or writes a history: the reset must precede the snapshot's disk sampling, bursts and merger transfers. No `pre_timestep` module touches a history, so any `pre_timestep` position satisfies it; first is the shipped position.
4. Reads the `PreviousType` that `sage26_record_transition_state` wrote as the last `post_timestep` module of the previous snapshot; no module between them writes it.

Besides its placement (Processing Contract), this module's `init()` checks none of these. A run without `sage26_record_transition_state` in `post_timestep` as `process_full_halo` is not refused: `init()` warns and the run proceeds with the clocks assigned and the infall state unrecorded. Its position among the `pre_timestep` modules (items 2 and 3) is not checked by any module. The partner checks are other modules': `sage26_resolve_mergers_and_disruption`'s for item 1, and `sage26_finalize_sfr_histories`'s `init()`, which refuses a run without this module in `pre_timestep` as `process_full_halo` (its history reset).

## Differences From SAGE26

The first four are core-boundary items: the core owns the behaviour, and the package does not change it. The last is a package residual outside this module's writes, recorded with its reason.

- **Raw halo inputs.** SAGE26 reads its float halo catalogue (`Mvir`, `Len`, `SnapNum`); the package reads the core's double `Mvir` of the satellite's and the FoF central's rows (the catalogue float widened, or `Len * PartMass`) and the snapshot redshift the core supplies. Where these equal SAGE26's inputs the clock is identical (the oracle test).
- **Previous Type.** Recorded by the package (`PreviousType`), not published by the core.
- **A Type 1 with a clock above 999 that loses its subhalo.** SAGE26 also records its `infallMvir`, `infallVvir` and `infallVmax` (`src/core_build_model.c:331-333`); the core records them only for a previous Type 0. The package records `infallStellarMass` and the zero clock. Every Type 1 holds an estimated clock (998 at most, or -1), so R0 never reaches this.
- **Orphan `Len`.** The core zeros a new orphan's `Len`; SAGE26 keeps it. The clock never reads an orphan's `Len`.
- **A new orphan's `Cooling`, `Heating`, `QuasarModeBHaccretionMass` and outflow rate.** SAGE26 resets them only on the main branch (`src/core_build_model.c:273-276`), so a new orphan keeps them; the package's `init_repeat` resets them on every inherited row. All four are package properties with `init_repeat: true` (`models/sage26/model_properties.yaml`, `SupernovaOutflowRate` being SAGE26's `OutflowRate`), a reset this module does not own. None of them is read by the pass or moved by a merger or disruption, and an orphan is removed within its snapshot, so no written row carries the difference on R0.

`estimate_merging_time`'s same-halo branch cannot be reached from joining (the satellite's halo is never the FoF's first), and the package keeps it as SAGE26 has it. The package's earlier zero-`Len` guard before the Coulomb logarithm is gone: SAGE26 forms the logarithm first (a zero satellite `Len` gives +inf, or NaN with a zero mother `Len`, without a trap), and its `Len >= 10` guard then returns -1, so the result is the same.

## Tests

- `_tests/test_unit_sage26_initialise_merger_clock_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) bit for bit: `estimate_merging_time` through the shared helper, the joining transcription through the core's inheritance service followed by this module (the clock, the infall state and the history reset, with a new orphan's histories kept), and the joining-and-pass probe followed by the package's merger pass. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- `_tests/test_unit_sage26_initialise_merger_clock.c` covers the placement check, `MergerTimeFactor`, and each transition on hand-built workspaces.
- `_tests/test_integration_sage26_initialise_merger_clock.py` covers the module in a run, its placement check and the parameter.
- `sage26_record_transition_state`'s unit and integration tests follow the transitions across two snapshots through the core's inheritance, and over consecutive snapshots of a real run.
