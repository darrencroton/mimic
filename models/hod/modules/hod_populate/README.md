# `hod_populate`

Populates each FoF group's Type 0 host with a five-parameter threshold-sample halo occupation distribution (HOD): a central drawn from a smoothed step in halo mass and, given a central, a Poisson number of satellites created as galaxy records at NFW radii. A whole-box audit compares the realised population with the analytic expectation. The package README (`models/hod/README.md`) carries the science scope and caveats.

## Processing Contract

- Supported modes: `process_full_halo` and `process_snapshot` (dual mode).
- `process_full_halo` must be configured exactly once in `modules.post_timestep` (checked at init): it runs once per FoF step, so every processed snapshot sees one retirement, one reset and at most one draw per host. Do not also configure it in `pre_timestep` or a substep phase: init does not reject that, but the module would retire and redraw the FoF step again (once per substep in a substep phase).
- `process_snapshot` must be configured in `modules.post_snapshot` whenever that phase exists. The phase exists only under the horizontal driver; a vertical run file has no `post_snapshot` and runs without the audit.
- The host is the row at `ctx->central_index`, which must be a Type 0 row with a galaxy; `ngal >= 1` is required. Every non-Type 3 row must have a galaxy. Each satellite is created with `module_create_record()` on that host, so the module must stay `process_full_halo`.
- All validation, the occupation draw and the identity-radix checks run before the first write, so a failed call leaves the FoF workspace untouched and the run stops with the module's error.

## Phases and Lifecycle

Per FoF step (`post_timestep`, every processed snapshot):

1. Every Type 2 row, including last snapshot's synthetic satellites inherited as orphans, is retired to Type 3, so no satellite outlives its snapshot.
2. `HODGhost = 1` on every row with a galaxy.
3. Only when the snapshot is in `output.snapshot_list` (an empty list means every snapshot): with `M = Mvir * 1e10` in `Msun/h`, the central is present iff `u0 < <Ncen>(M)`; a present central sets the host's `HODGhost = 0`. Given a central and `M > 10^HODLogM0`, `Nsat ~ Poisson(lambda)`; each satellite is created, placed, and given `HODGhost = 0`. Type 1 rows stay `HODGhost = 1`.

Per snapshot (`post_snapshot`, output snapshots only): the audit below. Non-output snapshots return without logging.

## Prescription

- **Occupation** (Zheng et al. 2005; Zheng, Coil & Zehavi 2007 equations 2 and 5): `<Ncen> = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]`; `lambda = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha` for `M > 10^HODLogM0`, else 0. Satellites are never drawn without a central (the central-gating convention), so the mean total occupation is `<Ncen> (1 + lambda)`.
- **Poisson draw**: inversion from one uniform with a sequential cumulative search from `k = 0`, each term formed in logarithms.
- **Identity radix**: `lambda >= 1024`, or a drawn `Nsat > 1024`, fails the module with the host's `UniqueGalaxyID` and `lambda` in the message. 1024 is `MAX_CREATED_RECORDS_PER_HOST`, the created-record identity radix; the count is never clipped.
- **Concentration** (Duffy et al. 2008, Table 1 form): `c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC`, `z = ctx->redshift`.
- **Radius**: `r_phys = Rvir x / c`, where `x` solves `m(x) / m(c) = u` with `m(x) = ln(1 + x) - x / (1 + x)`, by bisection on `[0, c]` to `|m(x)/m(c) - u| <= 1e-12`. `Rvir` is the host row's core field, a physical length.
- **Direction and position**: `cos theta = 2u - 1`, `phi = 2 pi u`; the comoving offset is `r_com = r_phys (1 + z)`; `Pos = host Pos + r_com n`, each component wrapped into `[0, BoxSize)` with exactly `BoxSize` mapping to 0. The wrap is done in double; a value that rounds up to `BoxSize` when stored as a float is stored as the largest float below `BoxSize`.
- **Velocity**: `Vel = host Vel + (g1, g2, g3) Vvir / sqrt(2)` with independent standard Gaussians; `Vvir` is the host row's core field.
- **Precision**: all of the above is evaluated in double; `Pos` and `Vel` are stored as the catalogue's floats.
- **Random numbers** (`models/hod/shared/hod_random.h`): one counter-based stream per host keyed by `(HODSeed, snapshot number, host UniqueGalaxyID)`, with fixed draw indices (`hod_populate.h`): 0 for the central, 1 for the satellite count, and 16 per satellite from index 2 (radius, `cos theta`, `phi`, three Box-Muller pairs). A host's draws therefore do not depend on row order, FoF order, traversal or partitioning, and repeat bitwise for identical input and parameters.
- **Created rows** start as copies of the host through the record-creation contract (Type 2, `Mvir = Len = 0`, `Rvir`/`Vvir` kept, infall values from the host, host `UniqueCentralGalaxyID`, a negative created `UniqueGalaxyID`, a fresh galaxy). Only `Pos`, `Vel` and `HODGhost` are set by this module; every other halo-side field (`MostBoundID`, `Spin`, `Vmax`, `VelDisp`) is the host's and does not distinguish a satellite from its host.

## Audit

On each output snapshot the snapshot callback reads the whole population (never writing) and logs at INFO:

```text
HOD audit z=<z> hosts=<n> n_gal expected=<x> realised=<y> f_sat expected=<a> realised=<b>
```

- `hosts`: Type 0 rows. `n_gal expected = sum over Type 0 rows of (<Ncen> + <Ncen> lambda) / V`, `realised = count(HODGhost == 0) / V`, with `V = BoxSize^3` in `(Mpc/h)^3`; both are `(Mpc/h)^-3`.
- `f_sat`: satellites' share of the sample, expected `sum <Ncen> lambda / sum <Ncen> (1 + lambda)` and realised `count(Type 2, HODGhost == 0) / count(HODGhost == 0)`; 0 when its denominator is 0.

With `--verbose` it also logs one line per occupied 0.2 dex host-mass bin over the host mass range:

```text
HOD audit bin z=<z> log10M=[<lo>, <hi>) hosts=<n> <N> expected=<x> realised=<y>
```

where `<N>` is the mean total occupation per host: a present central counts in its host's bin and a satellite in the bin of the Type 0 row whose `UniqueGalaxyID` is its `UniqueCentralGalaxyID`. A sample row that is neither a Type 0 central nor a Type 2 satellite, or a satellite with no such host, fails the audit. Scratch (the bins and an ID-sorted host table, `MEM_UTILITY`) is released before the callback returns.

## Properties

- Reads: `Type`, `Mvir`, `Rvir`, `Vvir`, `Pos`, `Vel`, `UniqueGalaxyID`, `HODGhost`; the audit also reads `UniqueCentralGalaxyID` to attribute each satellite to its host's mass bin
- Writes: `Type` (Type 2 to Type 3), `HODGhost`; on created rows `Pos`, `Vel`, `HODGhost`

## Parameters

All ten are required in `modules.parameters`; every double must be finite (`nan`, `inf` and overflowing strings such as `1e400` are rejected, as are malformed strings such as `1,0` and `abc`).

| Parameter | Units | Domain | Meaning |
|---|---|---|---|
| `HODLogMmin` | log10(Msun/h) | finite | Mass where `<Ncen> = 0.5` |
| `HODSigmaLogM` | dex | `> 0` | Width of the central step |
| `HODLogM0` | log10(Msun/h) | finite, `10^x` a finite positive normal double | Satellite cutoff mass |
| `HODLogM1` | log10(Msun/h) | finite, `10^x` a finite positive normal double | Satellite normalisation mass (M1') |
| `HODAlpha` | dimensionless | `>= 0` | Satellite slope |
| `HODSeed` | integer | `>= 0` | Random-number seed |
| `HODConcA` | dimensionless | `> 0` | Concentration at the pivot mass and z = 0 |
| `HODConcLogMpivot` | log10(Msun/h) | finite, `10^x` a finite positive normal double | Concentration pivot mass |
| `HODConcB` | dimensionless | finite | Concentration mass slope |
| `HODConcC` | dimensionless | finite | Concentration `(1 + z)` slope |

`BoxSize` comes from the simulation package and must be finite and positive with a finite cube.

## Tests

`_tests/test_unit_hod_populate.c` (21 cases) checks the random-number header, the spot values of every formula, the init guards and parameter domains, the lifecycle through the registered dispatch over hand-built FoF workspaces (retirement, reset, gating, created rows equal to the helper-level draws, the output-snapshot gate, the identity-radix errors), bitwise repeat identity and row/FoF permutation invariance, statistics over 40,000 independently keyed hosts, and the audit against a hand sum. Run it by path with the selectors exported, for example:

```bash
MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh \
  models/hod/modules/hod_populate/_tests/test_unit_hod_populate.c
```

## Notes

- Retirement covers every Type 2 row, so tree-born orphans also leave the sample; the HOD population at each output snapshot is entirely the host draws.
- A non-output snapshot still retires and resets; it creates nothing, so only output snapshots carry satellites.
- Created IDs are deterministic for a fixed dataset and run file but differ between drivers.

## References

Zheng et al. (2005), ApJ 633, 791; Zheng, Coil & Zehavi (2007), ApJ 667, 760; Duffy et al. (2008), MNRAS 390, L64; Berlind & Weinberg (2002), ApJ 575, 587.
