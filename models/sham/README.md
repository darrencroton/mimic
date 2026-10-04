# SHAM Model Package

This package assigns stellar masses to halo catalogues by subhalo abundance matching (SHAM): it ranks the resolved halos and subhalos of a snapshot by a halo property and gives each the stellar mass at which an observed stellar mass function reaches the same number density, so that the most massive galaxies live in the halos with the largest proxy. There is no star formation, gas or merger physics; the stellar mass is a function of the halo's rank alone.

The package has one module, `sham_rank_match`, and runs only under the horizontal driver, because the rank needs the whole snapshot at once (the `modules.post_snapshot` phase). Its shipped run file targets the committed micro-Uchuu horizontal fixture. See the [Developer Guide](../../docs/DEVELOPER-GUIDE.md#creating-physics-modules) for the model-package pattern and the [sage16 package](../sage16/README.md) for a physics model at production scale.

## Scientific Status

`sham_rank_match` is a calibrated-target rank match with these deliberate limits:

- **Cross-sectional.** One observed target, measured at low redshift, is applied only to output snapshots inside a declared redshift window (`ShamTargetRedshiftMax`); startup fails if any output snapshot lies outside it. There is no redshift evolution of the target.
- **Zero scatter.** Stellar mass is a monotonic function of rank; there is no intrinsic scatter between the proxy and stellar mass, and therefore no deconvolution.
- **Resolved only.** Candidates are Type 0 (FoF central) and Type 1 (resolved subhalo) rows. Orphans (Type 2 rows whose subhalo is no longer resolved) are retired before every FoF step, so a galaxy leaves the sample when its subhalo does.
- **No observational parity claim.** The target is reproduced by construction on the ranked population; nothing here claims that the resulting clustering, satellite fractions or stellar-to-halo relation match observations. The shipped run file uses a small synthetic fixture that is not a complete cosmological volume.

The module README (`modules/sham_rank_match/README.md`) is the exact contract, including the numerical and failure rules.

## Target and Conventions

The target is the Baldry et al. (2012) GAMA `z < 0.06` double Schechter stellar mass function (Chabrier IMF, stated validity `10^8` to `10^11.5 Msun`):

```text
Phi(M) dM = exp(-M/Ms) [phi1 (M/Ms)^alpha1 + phi2 (M/Ms)^alpha2] dM/Ms
log10 Ms = 10.66, phi1 = 3.96e-3, alpha1 = -0.35, phi2 = 0.79e-3, alpha2 = -1.47
```

with `Ms` in physical `Msun` and `phi` in physical `Mpc^-3`, both for the paper's `h = 0.7` (`ShamTargetHubble`). Observed stellar masses scale as `h^-2` and number densities as `h^3`, so at init the module converts the published values to the simulation's `h_sim` (`Hubble_h`):

```text
log10 Ms_sim = ShamTargetLogMstar + 2 log10(h_obs / h_sim)
phi_i_sim    = phi_i (h_sim / h_obs)^3
```

For micro-Uchuu (`h_sim = 0.6774`) this gives `log10 Ms_sim = 10.68851`, `phi1_sim = 3.58870e-3` and `phi2_sim = 7.15927e-4 Mpc^-3`. The cumulative density `n(>M) = integral_M^(120 Ms) Phi dM'` is tabulated once and inverted by bisection with log-log interpolation; at `h_sim = 0.6774`, `n(>10^8) = 3.031993e-2` and `n(>10^11.5) = 2.793662e-6 Mpc^-3`. Above the published fit range the analytic form is evaluated as written, up to `120 Ms`.

Units at the boundary: the simulation's `BoxSize` is comoving `Mpc/h`, so the physical rank density of zero-based rank `r` is `n_r = (r + 0.5) / BoxSize^3 * h_sim^3` in `Mpc^-3`; the matched mass `M*` is physical `Msun`, stored as `StellarMass = M* h_sim / 1e10` in Mimic's internal `1e10 Msun/h`.

## Candidates and Peak Proxy

The ranking proxy is `ShamVpeak`, the largest `Vmax` a row's galaxy has had along its resolved branch (Reddick et al. 2013 favour peak circular velocity because it is set before stripping). `ShamMpeak` tracks the peak `Mvir` the same way and is output for analysis but does not enter the rank. Both are accumulated at every processed snapshot, output or not, so the history before the first output snapshot counts.

A candidate is a Type 0 or Type 1 row with `ShamVpeak >= ShamMinVpeak`, a completeness floor in `km/s` set to where the catalogue's subhalos are resolved (80 km/s for micro-Uchuu). Candidates are ranked across the whole snapshot, every FoF group together, by descending `ShamVpeak`; exact ties go to the lower `UniqueGalaxyID`, so the result is deterministic and independent of row and FoF order.

## Masking Rule

The target is not extrapolated below its validity floor. If a candidate's rank density exceeds `n(>10^ShamTargetLogMassFloor)` (the density at the floor mass, a physical mass at the simulation's `h`), that candidate and every lower rank are masked: they keep `StellarMass = 0` and `ShamGhost = 1`. The masked count is reported, never hidden. On a complete volume whose candidate density stays below the floor density, nothing is masked and the ranked population reaches down to the mass at which the target's cumulative density equals the candidate density.

## Output Contract

Every row stays in output so halo catalogues remain complete. Consumers select the matched sample with `ShamGhost == 0`:

| Property | Units | Meaning |
|---|---|---|
| `StellarMass` | `1e10 Msun/h` | Matched stellar mass for sample members; 0 otherwise |
| `ShamGhost` | dimensionless | 0 = rank-matched sample member; 1 = not a sample member (below `ShamMinVpeak`, masked, or at a non-output snapshot) |
| `ShamVpeak` | km/s | Peak `Vmax` along the branch (the ranking proxy) |
| `ShamMpeak` | `1e10 Msun/h` | Peak `Mvir` along the branch |

Each output snapshot logs one INFO line, `SHAM audit z=<z> candidates=<n> assigned=<a> masked=<m>`, so the completeness of the ranked population is read from the run rather than assumed.

## Parameters

All nine are required in `modules.parameters` and must be finite.

| Parameter | Units | Domain | Meaning |
|---|---|---|---|
| `ShamTargetLogMstar` | log10(Msun) at `h_obs` | finite | Schechter mass `Ms` |
| `ShamTargetPhi1` | Mpc^-3 at `h_obs` | `> 0` | First normalisation |
| `ShamTargetAlpha1` | dimensionless | finite | First faint-end slope |
| `ShamTargetPhi2` | Mpc^-3 at `h_obs` | `> 0` | Second normalisation |
| `ShamTargetAlpha2` | dimensionless | finite | Second faint-end slope |
| `ShamTargetHubble` | dimensionless | `(0, 2)` | `h_obs` of the published fit |
| `ShamTargetLogMassFloor` | log10(Msun) at `h_sim` | `10^x < 120 Ms_sim` | Lowest assignable mass; denser ranks are masked |
| `ShamTargetRedshiftMax` | dimensionless | finite | Largest output-snapshot redshift the target may be applied at |
| `ShamMinVpeak` | km/s | `> 0` | Candidate completeness floor on `ShamVpeak` |

No parameter is converted through `parameter_units.yaml`; the package has none.

## Package Contents

- `model_properties.yaml`: `StellarMass`, `ShamVpeak`, `ShamMpeak` and `ShamGhost`.
- `input/sham_micro-uchuu-ascii-horizontal.yaml`: the fixture run file (Baldry et al. 2012 target, `ShamTargetLogMassFloor 8.0`, `ShamMinVpeak 80`, and a widened `ShamTargetRedshiftMax 0.2` so every fixture snapshot, `z = 0.1943` to `0.0005`, is assigned).
- `modules/sham_rank_match/`: the module, its README, its unit and integration tests and the independent reference `_tests/sham_rank_match_reference.py`.
- `modules/_tests/sham_test_fixtures.h`: the package's C test fixture (shipped parameters, `BoxSize` and `h` of the fixture).
- `plots/`: diagnostic figures for `mimic-plot.py`. They predate `sham_rank_match` and are being rewritten; some still read properties this package no longer declares.

## Build, Run, and Test

The model is horizontal-only. Build the fixture pair and run from the repository root (the run file's `simulation.config` is repository-relative); output lands in `output/sham-micro-uchuu-ascii-horizontal/`:

```bash
make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal
./mimic models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml
```

Validation and the module's unit test (run by path with the selectors exported, since the runner otherwise falls back to the default pair):

```bash
make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal validate-modules lint-parameters check-generated
make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes mimic
MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh \
  models/sham/modules/sham_rank_match/_tests/test_unit_sham_rank_match.c
mimic_venv/bin/python3 models/sham/modules/sham_rank_match/_tests/test_integration_sham_rank_match.py
```

The integration test currently holds one pure-Python case, which requires the unit test's reference table to equal what `sham_rank_match_reference.py` computes; it needs no executable and runs under any pair.

## Follow-ups

- **Scatter.** Intrinsic log-normal scatter between the proxy and stellar mass, with the target deconvolved so the scattered population still reproduces it, is the recorded next extension.
- **Orphans.** An orphan-inclusive mode would keep Type 2 rows as candidates for a limited time after their subhalo is lost, which matters for small-scale clustering in catalogues with limited resolution.

## References

- Conroy, Wechsler & Kravtsov (2006), ApJ 647, 201: subhalo abundance matching and the ranking construction
- Vale & Ostriker (2006), MNRAS 371, 1173: matching luminosity functions to (sub)halo mass functions
- Behroozi, Conroy & Wechsler (2010), ApJ 717, 379: abundance matching and its uncertainties, including scatter
- Reddick et al. (2013), ApJ 771, 30: peak circular velocity as the ranking proxy and the role of scatter
- Baldry et al. (2012), MNRAS 421, 621: the GAMA `z < 0.06` double Schechter stellar mass function used as the target
