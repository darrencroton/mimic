# HOD Model Package

This package populates halo catalogues with a halo occupation distribution (HOD): a statistical prescription that assigns each host halo a number of galaxies from its mass alone, with no star formation, gas or merger physics. It is a threshold-sample HOD: the galaxies it creates are members of a sample brighter than a luminosity threshold (by default the SDSS `Mr < -20` sample of Zheng, Coil & Zehavi 2007), and they carry positions and velocities but no stellar mass.

The package is a demonstration of Mimic's record-creation contract (a module adding galaxy records to the FoF group it processes) and ships with published parameters. It does not solve for the minimum mass in-run, does not use subhalo positions for satellites, carries no assembly bias or secondary-property dependence, and makes no observational parity claim; see [Caveats](#caveats).

## Scientific Scope

- **Hosts**: each FoF group's Type 0 row (the FoF central). Type 1 rows (resolved subhalos) are catalogue scaffold, never hosts.
- **Occupation law** (Zheng et al. 2005; Zheng, Coil & Zehavi 2007 equations 2 and 5), with `M = Mvir * 1e10` in `Msun/h`:
  - `<Ncen>(M) = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]`
  - `lambda(M) = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha` for `M > 10^HODLogM0`, else 0
- **Gating convention**: a host has a central with probability `<Ncen>`, and only a host with a central draws satellites, `Nsat ~ Poisson(lambda)`. The mean total occupation is `<N(M)> = <Ncen> (1 + lambda)`. A host never has satellites without a central.
- **Placement**: satellites sit at NFW radii inside the host's virial radius with the Duffy et al. (2008) concentration `c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC`. The radius solves `m(x) / m(c) = u` with `m(x) = ln(1 + x) - x / (1 + x)` and `r_phys = Rvir x / c`. Directions are isotropic. Because `Rvir` is a physical length and `Pos` is comoving, the offset is converted to comoving coordinates, `r_com = r_phys (1 + z)`, before it is added to the host position; each coordinate is then wrapped periodically into `[0, BoxSize)`.
- **Velocities**: host bulk velocity plus three independent Gaussians with `sigma_1D = Vvir / sqrt(2)`.
- **Lifetime**: synthetic satellites are regenerated at every output snapshot. Inherited satellites, and tree-born orphans, are retired before output, so no Type 2 row survives from one snapshot to the next.

The module README (`modules/hod_populate/README.md`) is the exact contract, including the precision and failure rules.

## Random Numbers and Determinism

Draws come from the model-private counter-based generator `shared/hod_random.h` (splitmix64 finaliser, uniforms strictly inside `(0, 1)`, Box-Muller Gaussians, Poisson by inversion). Each host has its own stream keyed by `(HODSeed, snapshot number, host UniqueGalaxyID)`, and every draw has a fixed index within it. Consequences:

- Repeating a run with the same dataset and run file gives bitwise-identical output.
- A host's realisation does not depend on the order of rows in its FoF group, the order of FoF groups, the driver's traversal or partitioning.
- Changing `HODSeed` gives an independent realisation.
- Created satellite IDs are deterministic but differ between the vertical and horizontal drivers, which number host rows differently.

## Output Contract: Scaffold and Ghosts

The package declares one galaxy property, `HODGhost` (int, `[0, 1]`). Every halo row of the catalogue stays in the output as scaffold, so the halo catalogue remains complete; the HOD sample is the rows with `HODGhost == 0`:

| Row | Type | `HODGhost` |
|---|---|---|
| Host whose central was drawn present | 0 | 0 |
| Host whose central was drawn absent | 0 | 1 |
| Resolved subhalo (scaffold) | 1 | 1 |
| Created satellite | 2 | 0 |

Satellites are Type 2 rows with negative created `UniqueGalaxyID`s and their host's `UniqueCentralGalaxyID`; they are written immediately after their host's subhalo slice. Their halo-side fields other than `Pos` and `Vel` (`Mvir = 0`, `Len = 0`, `Rvir`, `Vvir`, `Vmax`, `Spin`, `MostBoundID`) are copies of, or derived from, the host's and say nothing about the satellite. Consumers filter on `HODGhost == 0`. Snapshots outside `output.snapshot_list` are processed (retirement and reset) but not drawn, and are not written.

## Parameters

All are required in `modules.parameters`. Defaults shipped in the run file: the SDSS `Mr < -20` fit of Zheng, Coil & Zehavi (2007) and the Duffy et al. (2008) NFW full-sample 200c concentration (z 0-2).

| Parameter | Units | Default | Meaning |
|---|---|---|---|
| `HODLogMmin` | log10(Msun/h) | 12.02 | Mass where `<Ncen> = 0.5` |
| `HODSigmaLogM` | dex | 0.26 | Width of the central step (`> 0`) |
| `HODLogM0` | log10(Msun/h) | 11.38 | Satellite cutoff mass |
| `HODLogM1` | log10(Msun/h) | 13.31 | Satellite normalisation mass (M1') |
| `HODAlpha` | dimensionless | 1.06 | Satellite slope (`>= 0`) |
| `HODSeed` | integer | 1 | Random-number seed (`>= 0`) |
| `HODConcA` | dimensionless | 5.71 | Concentration at the pivot mass, z = 0 (`> 0`) |
| `HODConcLogMpivot` | log10(Msun/h) | 12.301030 | Pivot mass, log10(2e12) |
| `HODConcB` | dimensionless | -0.084 | Concentration mass slope |
| `HODConcC` | dimensionless | -0.47 | Concentration `(1 + z)` slope |

Hand-checkable values at these defaults: `<Ncen>(10^12.02) = 0.5`, `<Ncen>(1e14) = 1`; `lambda(1e13) = 0.457322`, `lambda(1e14) = 5.373959`, `lambda(1e15) = 61.842859`; `c(2e12, z=0) = 5.71`, `c(1e14, 0) = 4.110765`, `c(1e12, 1) = 4.369568`.

A host with `lambda >= 1024`, or one that draws more than 1024 satellites, stops the run with the host's ID and `lambda`: 1024 is the created-record identity radix per host, and the count is never clipped.

## Audit Log

Under the horizontal driver, `post_snapshot: hod_populate: process_snapshot` audits every output snapshot and logs at INFO:

```text
HOD audit z=<z> hosts=<n> n_gal expected=<x> realised=<y> f_sat expected=<a> realised=<b>
```

`hosts` is the number of Type 0 rows; `n_gal` is the sample number density in `(Mpc/h)^-3` over `V = BoxSize^3`, expected `sum <Ncen> (1 + lambda) / V` over the hosts and realised `count(HODGhost == 0) / V`; `f_sat` is the satellite fraction of the sample. With `--verbose`, one `HOD audit bin` line per occupied 0.2 dex host-mass bin compares the realised mean occupation `<N(M)>` with the expectation. The realised numbers scatter about the expected ones by the Poisson and binomial noise the host counts imply; on a small population they can differ substantially. The vertical driver has no `post_snapshot` phase, so a vertical run file omits it and runs without the audit.

## Caveats

- **Mass definition**: the Zheng, Coil & Zehavi (2007) fit uses halo masses defined at 200 times the mean density. Mimic's `Mvir` is the catalogue's virial mass (M200c for Millennium, the Rockstar virial mass for Uchuu). No conversion is applied.
- **Cosmology**: the fit assumed `Omega_m 0.3, sigma_8 0.9, h 0.7`; the catalogues have their own cosmology, which shifts the halo mass function under the fixed parameters.
- **Concentration**: the Duffy et al. (2008) relation is for 200c NFW fits over z 0-2; it is applied to the catalogue mass at any redshift.
- **Consequence**: the output demonstrates the framework with published parameters; it is not a calibrated SDSS mock, and the audit's densities are not a measurement of the SDSS sample.
- **Fixture**: `input/hod_micro-uchuu-ascii-horizontal.yaml` runs on the committed synthetic fixture (three forests over six snapshots in a 100 Mpc/h box), which is not a complete cosmological volume.

## Package Contents

- `model_properties.yaml`: the `HODGhost` galaxy property.
- `modules/hod_populate/`: the dual-mode module (`module_info.yaml`, `hod_populate.c`, `hod_populate.h`, `README.md`, `_tests/test_unit_hod_populate.c`).
- `modules/_tests/hod_test_fixtures.h`: shared C unit-test fixtures (counters, configuration reset, default parameters).
- `shared/hod_random.h`: the model-private random-number generator (`shared/README.md`).
- `input/hod_micro-uchuu-ascii-horizontal.yaml`: the fixture run file.

## Build, Run, and Test

One `MODEL`/`SIMULATION` pair per command sequence; the fixture run file needs the horizontal fixture package and runs from the repository root:

```bash
make MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal generate validate-modules lint-parameters
make MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal -j$(sysctl -n hw.ncpu)
./mimic models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml
```

Add `--verbose` to see the per-bin audit lines. Output is HDF5 under `output/hod-micro-uchuu-ascii-horizontal/`.

The module's unit test runs by path; export the selectors, because the standalone runner otherwise falls back to the project defaults:

```bash
MODEL=hod SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh \
  models/hod/modules/hod_populate/_tests/test_unit_hod_populate.c
```

Module tests register in the default tiers only for vertical simulations, so under `MODEL=hod SIMULATION=mini-millennium` the unit test also runs from `make tests-unit`.

## References

- Zheng, Z., et al. (2005), ApJ 633, 791 — the five-parameter occupation form.
- Zheng, Z., Coil, A. L., & Zehavi, I. (2007), ApJ 667, 760 — the SDSS `Mr < -20` fit (equations 2 and 5).
- Duffy, A. R., et al. (2008), MNRAS 390, L64 — the concentration-mass relation (Table 1).
- Berlind, A. A., & Weinberg, D. H. (2002), ApJ 575, 587 — the HOD framework and central/satellite decomposition.
