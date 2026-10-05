# HOD Model Package

This package populates halo catalogues with a halo occupation distribution (HOD): a statistical prescription that assigns each host halo a number of galaxies from its mass alone, with no star formation, gas or merger physics. It is a threshold-sample HOD: the galaxies it creates are members of a sample brighter than a luminosity threshold (by default the SDSS `Mr < -20` sample of Zheng, Coil & Zehavi 2007), and they carry positions and velocities but no stellar mass.

The package is a demonstration of Mimic's record-creation contract (a module adding galaxy records to the FoF group it processes) and ships with published parameters. It does not solve for the minimum mass in-run, does not use subhalo positions for satellites, carries no assembly bias or secondary-property dependence, and makes no observational parity claim; see [Caveats](#caveats).

## Scientific Scope

- **Hosts**: each FoF group's Type 0 row (the FoF central). Type 1 rows (resolved subhalos) are catalogue scaffold, never hosts.
- **Occupation law** (Zheng et al. 2005; Zheng, Coil & Zehavi 2007 equations 2 and 5), with `M = Mvir * 1e10` in `Msun/h`:
  - `<Ncen>(M) = 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]`
  - `lambda(M) = ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha` for `M > 10^HODLogM0`, else 0
- **Gating convention**: a host has a central with probability `<Ncen>`, and only a host with a central draws satellites, `Nsat ~ Poisson(lambda)`. The mean total occupation is `<N(M)> = <Ncen> (1 + lambda)`. A host never has satellites without a central.
- **Placement**: satellites sit at NFW radii inside the host's current virial radius with the Duffy et al. (2008) concentration `c = HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC`. The radius solves `m(x) / m(c) = u` with `m(x) = ln(1 + x) - x / (1 + x)` and `r_phys = Rvir x / c`, where `Rvir` is the host's virial radius at the draw redshift, computed from its current `Mvir` with the core's critical-density formula (not the host row's own `Rvir` field, which inheritance keeps at the branch's maximum). Directions are isotropic. Because `Rvir` is a physical length and `Pos` is comoving, the offset is converted to comoving coordinates, `r_com = r_phys (1 + z)`, before it is added to the host position; each coordinate is then wrapped periodically into `[0, BoxSize)`.
- **Velocities**: host bulk velocity plus three independent Gaussians with `sigma_1D = Vvir / sqrt(2)`, `Vvir` the host's current virial velocity `sqrt(G Mvir / Rvir)`.
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

Satellites are Type 2 rows with negative created `UniqueGalaxyID`s and their host's `UniqueCentralGalaxyID`; they are written immediately after their host's subhalo slice. Their halo-side fields other than `Pos` and `Vel` (`Mvir = 0`, `Len = 0`, `Vmax`, `Spin`, `MostBoundID`) are copies of, or derived from, the host's and say nothing about the satellite; `Rvir` and `Vvir` are the host's current virial radius and velocity at the draw redshift, the values the placement used. Consumers filter on `HODGhost == 0`. Snapshots outside `output.snapshot_list` are processed (retirement and reset) but not drawn, and are not written.

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

`hosts` is the number of Type 0 rows; `n_gal` is the sample number density in `(Mpc/h)^-3` over `V = BoxSize^3`, expected `sum <Ncen> (1 + lambda) / V` over the hosts and realised `count(HODGhost == 0) / V`; `f_sat` is the satellite fraction of the sample. With `--verbose`, one `HOD audit bin` line per occupied 0.2 dex host-mass bin compares the realised mean occupation `<N(M)>` with the expectation. The realised numbers scatter about the expected ones by the Poisson and binomial noise the host counts imply; on a small population they can differ substantially. The vertical driver has no `post_snapshot` phase, so a vertical run file omits it and runs without the audit. Under several MPI tasks the module's collectives (`snapshot_distribution: collective`) make these whole-snapshot totals; the integer counts are exact, while the double sums depend on the reduction order and may differ from a single-process run in the last bits.

## Plots

`mimic-plot` draws five snapshot figures for this package (no evolution figures); the registry is `plots/figures/__init__.py`, the list is `plots/profiles/default.yaml` and the axes for the real micro-Uchuu box are in `plots/profiles/micro-uchuu-horizontal_plot_profile.yaml`. Every HOD figure selects the sample with `HODGhost == 0` and says so in its labels.

```bash
source mimic_venv/bin/activate
python plot/mimic-plot/mimic-plot.py --param-file models/hod/input/hod_micro-uchuu-horizontal.yaml
```

| Figure | What it shows |
|---|---|
| `hod_occupation` | Realised `<Ncen>`, `<Nsat>` and `<Ntot>` against `log10 Mvir [Msun/h]` in 0.2 dex host-mass bins (bins with at least 10 hosts), over the analytic curves computed from the run file's `modules.parameters`. The error bars are the standard error of the bin mean expected under the law: binomial for the central (`p (1 - p)` per host), and for satellites the Poisson-gated count (`Nsat ~ Poisson(lambda)` only for a host with a central), `Var(Nsat) = p lambda + p (1 - p) lambda^2`. |
| `hod_satellite_profile` | Two panels. Left: the cumulative distribution of the satellites' physical radius in units of `Rvir` (`r_com / ((1 + z) Rvir)`, `r_com` the minimum-image distance to the host) against the satellite-weighted average of each host's own NFW enclosed-mass fraction at its concentration. Right: the three satellite-minus-host velocity components divided by `Vvir / sqrt(2)` against the unit Gaussian, normalised by the whole sample so that a profile window cutting the tails (`axes.hod_satellite_profile` `xmin`/`xmax`; `ymin`/`ymax` set the velocity panel's linear y range) still compares with the unconditional Gaussian. `Rvir` and `Vvir` are the satellite row's own, which `hod_populate` writes as the host's current virial radius and velocity at the draw redshift, the values the placement used and the ones the host row's output carries (the concentration likewise uses the row's `infallMvir`, the host's `Mvir` at the draw, because the row's `Mvir` is zero). The snapshot redshift comes from the package's `a_list`. |
| `hod_correlation_function` | Real-space `xi(r)` of the sample on logarithmic bins from 0.1 to 20 Mpc/h (profile keys `xmin`/`xmax`, at most half the box) from the engine's `correlation_function`, with Poisson error bars, annotated with the realised number density and satellite fraction. Bins with pairs but `xi <= 0` cannot be drawn on the logarithmic axis and are counted in a note on the figure. Positions are wrapped into the box before counting. Real-space only: there is no projected `wp(rp)` and no observational overlay. |
| `halo_mass_function`, `spatial_distribution` | Copied unchanged from `halos-only`: the halo mass function and the halo positions, which give the occupation and clustering figures their context. |

A figure whose data are absent returns a skip reason instead of failing: the three HOD figures need `HODGhost`, `hod_satellite_profile` needs created satellites and one snapshot in the data, and `hod_correlation_function` needs the whole box. Check on the real micro-Uchuu run (z = 0.0005, 5,359 sample members): 14 bins have at least 50 hosts, and in all of them each of `<Ncen>`, `<Nsat>` and `<Ntot>` lies within 3 standard errors of the law averaged over the bin's own hosts (largest pull 1.82; largest absolute difference 0.122 in `<Nsat>` and `<Ntot>` at `log10 M = 13.5`, 56 hosts).

## Caveats

- **Mass definition**: the Zheng, Coil & Zehavi (2007) fit uses halo masses defined at 200 times the mean density. Mimic's `Mvir` is the catalogue's virial mass (M200c for Millennium, the Rockstar virial mass for Uchuu). No conversion is applied.
- **Cosmology**: the fit assumed `Omega_m 0.3, sigma_8 0.9, h 0.7`; the catalogues have their own cosmology, which shifts the halo mass function under the fixed parameters.
- **Concentration**: the Duffy et al. (2008) relation is for 200c NFW fits over z 0-2; it is applied to the catalogue mass at any redshift.
- **Virial scale**: placement uses the host's virial radius and velocity at the draw redshift from its current `Mvir`, because Mimic's inheritance keeps a host row's `Rvir` and `Vvir` at the largest value its branch ever had. On the real micro-Uchuu box at z = 0.0005 the peak-mass values differed from the current ones for 32.1% of the 994 satellites (ratio to the host's current `Rvir` from 0.943 to 1.207, mean 0.9994; velocity ratio up to 1.318); the current values now agree for every satellite, and the satellite-profile figure's maximum CDF difference (0.0285) and velocity standard deviation (1.0156 in units of `Vvir / sqrt(2)`) are unchanged because they are normalised by each row's own `Rvir` and `Vvir`.
- **Consequence**: the output demonstrates the framework with published parameters; it is not a calibrated SDSS mock, and the audit's densities are not a measurement of the SDSS sample.
- **Fixture**: `input/hod_micro-uchuu-ascii-horizontal.yaml` runs on the committed synthetic fixture (three forests over six snapshots in a 100 Mpc/h box), which is not a complete cosmological volume.
- **Drivers**: the audit needs the horizontal driver; `input/hod_mini-millennium.yaml` runs vertically without it. Created `UniqueGalaxyID`s are not identical across drivers. A pair whose created-record identity space does not fit int64 (the vertical driver on Shin-Uchuu ASCII and on full Uchuu) fails at the first satellite with the core's refusal, after the retirement and ghost reset of that step have been written; use the horizontal driver where a horizontal package exists.

## Package Contents

- `model_properties.yaml`: the `HODGhost` galaxy property.
- `modules/hod_populate/`: the dual-mode module (`module_info.yaml`, `hod_populate.c`, `hod_populate.h`, `README.md`, `_tests/test_unit_hod_populate.c`, `_tests/test_integration_hod_populate.py`).
- `modules/_tests/hod_test_fixtures.h`: shared C unit-test fixtures (counters, configuration reset, default parameters).
- `shared/hod_random.h`: the model-private random-number generator (`shared/README.md`).
- `input/hod_micro-uchuu-ascii-horizontal.yaml`: the fixture run file (horizontal driver, with the audit).
- `input/hod_micro-uchuu-horizontal.yaml`: the real 100 Mpc/h micro-Uchuu box (horizontal driver, with the audit); needs `simulations/micro-uchuu-horizontal/snapshots`.
- `plots/`: the figure registry, five figures and the plot profiles (see [Plots](#plots)).
- `input/hod_mini-millennium.yaml`: the eight local mini-Millennium tree files (vertical driver, no `post_snapshot` phase and so no audit, HDF5 output).

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

Module tests register in the default tiers only for the simulations in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`: `mini-millennium`, `micro-uchuu`, `micro-uchuu-hdf5` and `micro-uchuu-ascii`), so under `MODEL=hod SIMULATION=mini-millennium` the unit test also runs from `make tests-unit`, and `make MODEL=hod SIMULATION=mini-millennium tests-integration` runs the integration test's vertical cases on the eight tree files (its fixture cases report a configuration skip there). The fixture cases need the horizontal fixture build; `make tests-snapshot-global-hod` builds it as a test build and runs the unit test and those cases (also run by `make tests-snapshot-global`, which restores your generated code afterwards: rebuild with `make`).

The integration test (`modules/hod_populate/_tests/test_integration_hod_populate.py`) reads the HDF5 output per `UniqueGalaxyID` and checks that every created row is a Type 2 row with a negative ID, `HODGhost == 0` and a Type 0 `HODGhost == 0` host (so a host with `HODGhost == 1` has no satellite), that every Type 1 row is scaffold, that no Type 2 row survives from an earlier snapshot, that repeated runs are bitwise identical, that `HODSeed` changes the draws, that a run listing only the last fixture snapshot reproduces its rows bitwise, that conflicting phase configurations fail at startup and that every run is leak-free. The shipped Mr < -20 parameters give the fixture's low-mass hosts (10^11.2 to 10^12 Msun/h) almost no satellites, so the created-row cases run a temporary copy of the fixture run file with `HODLogMmin 11.8`, `HODLogM0 10.0` and `HODLogM1 10.8`; the shipped run file keeps the published values and has its own validity case. The fixture has no Type 1 rows, so the Type 1 assertion is exercised on mini-Millennium.

### Real-data runs

```bash
make MODEL=hod SIMULATION=micro-uchuu-horizontal generate validate-modules
make MODEL=hod SIMULATION=micro-uchuu-horizontal -j$(sysctl -n hw.ncpu)
./mimic models/hod/input/hod_micro-uchuu-horizontal.yaml
```

`input/hod_micro-uchuu-horizontal.yaml` runs the real 100 Mpc/h micro-Uchuu box (50 snapshots, `snapshot_list: [49, 28, 23, 16, 12, 10, 8, 7]` as in the `halos-only` sibling) under the horizontal driver, single process and fully resident. Real-data measurement (2026-10-04, macOS, default optimised build, `/usr/bin/time -l`). Switching the placement from the host row's peak-mass `Rvir` and `Vvir` to the current values left every count, audit line and the written-row total unchanged (the draws of the occupation do not depend on the virial radius) and changed only the satellites' positions and velocities; the first measurement with peak-mass values took 9.1 s:

| Quantity | Value |
|---|---|
| Wall clock | 7.7 s (4.1 s user, 0.5 s system) |
| Peak process RSS | 2.23 GB (the run's own memory profile line: `Peak process RSS: 2.231 GB`) |
| Output buffer | peak per-snapshot occupancy 616,184 records (the run profile's running maximum, retired rows included) in a 652,428-record buffer at 184 B |
| Written output | 3,114,016 `Galaxies` rows over the eight snapshots (the sum of the eight datasets in the recorded output, 10,986 of them sample members with `HODGhost == 0`) |
| Created-record identity space | `units=50 rows_per_unit=621360 radix=1024`, fits int64 |

Audit lines, with `V = 1e6 (Mpc/h)^3` so the expected and realised counts are `n_gal V`; the pull is `(realised - expected) / sqrt(expected)`. That normalisation is approximate: under central gating a host's variance is `p (1 - p) (1 + lambda)^2 + p lambda` with `p = <Ncen>`, not the Poisson `p (1 + lambda)` that `sqrt(expected)` assumes, so the scatter the expected count implies is only a guide:

| z | hosts | expected N | realised N | f_sat expected | f_sat realised | pull |
|---|---|---|---|---|---|---|
| 7.0257 | 82,192 | 0.03 | 0 | 0.003093 | 0 | -0.18 |
| 6.3416 | 127,465 | 0.6 | 1 | 0.009054 | 0 | +0.56 |
| 5.1546 | 240,822 | 22.0 | 26 | 0.028810 | 0 | +0.86 |
| 4.2670 | 347,231 | 102.1 | 105 | 0.040028 | 0.066667 | +0.28 |
| 3.1278 | 472,307 | 586.4 | 587 | 0.053399 | 0.054514 | +0.02 |
| 2.0293 | 540,495 | 1893.3 | 1874 | 0.078751 | 0.076307 | -0.44 |
| 1.4259 | 538,694 | 3003.9 | 3034 | 0.101634 | 0.102505 | +0.55 |
| 0.0005 | 496,374 | 5341.1 | 5359 | 0.181184 | 0.185482 | +0.25 |

Every pull is within one approximate standard deviation; the realised satellite fractions track the expectation where the counts are large (0.185 against 0.181 at z = 0.0005) and are noise where they are not. The z = 0.0005 row is the nearest to a population comparable to the SDSS sample, but see [Caveats](#caveats): the number density (expected 5.34e-3, realised 5.36e-3 (Mpc/h)^-3) is the framework's output for the published parameters on this catalogue's masses, not a measurement of the SDSS sample.

## References

- Zheng, Z., et al. (2005), ApJ 633, 791 — the five-parameter occupation form.
- Zheng, Z., Coil, A. L., & Zehavi, I. (2007), ApJ 667, 760 — the SDSS `Mr < -20` fit (equations 2 and 5).
- Duffy, A. R., et al. (2008), MNRAS 390, L64 — the concentration-mass relation (Table 1).
- Berlind, A. A., & Weinberg, D. H. (2002), ApJ 575, 587 — the HOD framework and central/satellite decomposition.
