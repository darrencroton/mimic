# SAGE Merger/Disruption Immediate Ordering Parity Contract

Date: 2026-03-12
Status: Baseline contract for WS2

## Scope

This contract defines the SAGE behavior Mimic must reproduce for satellite
merger and disruption ordering within a substep. It is derived directly from:

- `sage-code/sage/core_build_model.c` (`handle_mergers(...)`, lines 489-548)
- `sage-code/sage/model_mergers.c` (`deal_with_galaxy_merger(...)`, lines 107-154)
- `sage-code/sage/model_mergers.c` (`disrupt_satellite_to_ICS(...)`, lines 536-560)

## Contract

### 1. Single-pass immediate loop

SAGE processes satellites in index order inside one loop. For each `p`, it:

1. requires `Type == 1 || Type == 2` and `mergeType == 0`,
2. decrements `MergTime` by `deltaT / STEPS`,
3. computes eligibility from live substep state,
4. resolves the execution target,
5. writes `mergeIntoID`,
6. executes disruption or merger immediately before advancing to `p + 1`.

There is no staging pass with deferred `IsMerging` or `IsDisrupting` flags.

## 2. Eligibility and timing

- `deltaT` is `Age[Gal[p].SnapNum] - Age[Halo[halonr].SnapNum]`.
- `currentMvir` is linearly interpolated within the substep before eligibility is
  checked.
- A satellite is eligible when it has zero baryons, or when
  `currentMvir / (StellarMass + ColdGas) <= ThresholdSatDisruption`.
- If eligible and `MergTime > 0.0`, SAGE disrupts immediately.
- If eligible and `MergTime <= 0.0`, SAGE merges immediately.
- Merger event time is `Age[Gal[p].SnapNum] - (step + 0.5) * (deltaT / STEPS)`.

## 3. Target resolution and redirect

- Type 1 satellites target the current FOF central passed to
  `handle_mergers(...)`.
- Type 2 satellites target `Gal[p].CentralGal`.
- If that chosen target already has `mergeType > 0`, SAGE redirects exactly one
  hop to `Gal[target].CentralGal`.
- This redirect is not recursive.
- This redirect is not a generic fallback to the FOF Type 0 central.

Parity consequence: Mimic must not substitute a Type-based FOF fallback where
SAGE follows the consumed target's recorded `CentralGal`.

## 4. Disruption semantics

`disrupt_satellite_to_ICS(...)` executes immediately and:

- transfers cold and hot gas to the target's `HotGas`,
- transfers ejected mass to the target,
- transfers existing `ICS` to the target,
- transfers all stellar mass to the target's `ICS`,
- leaves the disrupted satellite black hole with no transfer,
- marks only the disrupted satellite with `mergeType = 4`.

Parity consequence: later satellites see the target's mutated gas and ICS state
from earlier disruptions in the same loop.

## 5. Merger semantics

`deal_with_galaxy_merger(...)` executes immediately and:

- computes merger mass ratio against the target's live pre-transfer state,
- transfers all baryonic components into the target,
- applies merger-triggered black-hole growth,
- applies merger-triggered collisional starburst,
- updates `TimeOfLastMinorMerger` when `mass_ratio > 0.1`,
- for major mergers, calls `make_bulge_from_burst(...)`, sets
  `TimeOfLastMajorMerger`, and marks the satellite with `mergeType = 2`,
- otherwise marks the satellite with `mergeType = 1`.

Parity consequence: later satellites must see target state after any earlier
mass transfer, black-hole growth, and starburst effects already executed in the
same loop.

## 6. Mimic implementation rules implied by this contract

- A parity path must execute satellite decisions in one full-halo pass.
- Per-merger downstream consumers must run immediately after each merger event,
  not after all satellites are pre-flagged.
- A parity harness must include at least:
  - one shared-target `disrupt -> merge` fixture,
  - one consumed-target redirect fixture for a Type 2 satellite,
  - one borderline major/minor classification fixture where earlier mutations
    can change the later mass ratio.
