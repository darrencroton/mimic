# `sham_rank_match`

Subhalo abundance matching against a calibrated target: at each output snapshot it ranks the resolved halos and subhalos of the whole snapshot by peak maximum circular velocity and assigns `StellarMass` by matching each rank's number density to the Baldry et al. (2012) GAMA stellar mass function, converted to the simulation's Hubble parameter. It is cross-sectional (one target applied inside a declared redshift window), has zero scatter and ranks resolved rows only. The package README (`models/sham/README.md`) carries the science scope and caveats.

## Processing Contract

- Supported modes: `process_full_halo` and `process_snapshot` (dual mode).
- `process_full_halo` must be configured exactly once, in `modules.pre_timestep`, and in no other FoF phase: its retirement of carried Type 2 rows must precede every other module, and a second entry would repeat the reset.
- `process_snapshot` must be configured in `modules.post_snapshot`. That phase runs only under the horizontal driver, once per snapshot after every FoF group is processed, so the model is horizontal-only.
- The FoF callback validates every row (Type in `[0, 3]`, a galaxy on every row of Type 0-2, finite nonnegative `Vmax`, `Mvir` and peaks, a storable mass peak) before its first write, so a failed FoF step leaves the workspace untouched. It never allocates.
- The snapshot callback receives the borrowed population and never reorders or resizes it: ranking sorts module-owned scratch (one 32-byte record per entry, `MEM_UTILITY`), released before the callback returns on every path. All validation and every inversion run before the first write, so a failed snapshot assigns nothing. It retains no pointer and never indexes by `CentralHalo`.

## Phases and Lifecycle

Per FoF step (`pre_timestep`, every processed snapshot), on every row in this order:

1. A Type 2 row becomes Type 3: carried orphans leave the population, so the candidates are resolved rows only.
2. A Type 0/1 row updates `ShamVpeak = max(ShamVpeak, Vmax)` and `ShamMpeak = max(ShamMpeak, Mvir)`. The mass maximum is formed in double and must be at most `FLT_MAX` before it is stored as a float: `Mvir = FLT_MAX` is stored exactly, while `Mvir = 1e39` fails the FoF step with the `UniqueGalaxyID` and the value.
3. `ShamGhost = 1` and `StellarMass = 0` on every row with a galaxy.

Per snapshot (`post_snapshot`), on output snapshots only (non-output snapshots return 0 without ranking or logging):

1. Every entry must be Type 0 or 1 (the FoF callback retired every Type 2 before marshal, so a Type 2 or 3 entry means it did not run), carry a galaxy and a positive `UniqueGalaxyID` that is unique in the snapshot (checked in an ID-sorted scratch pass), and have finite nonnegative peaks. Any violation fails the snapshot with the `UniqueGalaxyID` and value.
2. Candidates are the entries with `ShamVpeak >= ShamMinVpeak`, sorted by descending `ShamVpeak`, exact ties by ascending `UniqueGalaxyID`. Zero-based rank `r` has the physical rank density `n_r = (r + 0.5) / BoxSize^3 * h_sim^3` in `Mpc^-3`.
3. If `n_r > n(>10^ShamTargetLogMassFloor)` the candidate and every lower rank are masked (`ShamGhost = 1`, `StellarMass = 0`). Otherwise `StellarMass = n^-1(n_r) * h_sim / 1e10` (internal `1e10 Msun/h`) and `ShamGhost = 0`. Entries below `ShamMinVpeak` are not written and keep the FoF callback's reset.
4. One line at INFO: `SHAM audit z=<z> candidates=<n> assigned=<a> masked=<m>`.

## Prescription

- **Target** (Baldry et al. 2012, GAMA z < 0.06, Chabrier IMF): `Phi(M) dM = exp(-M/Ms) [phi1 (M/Ms)^alpha1 + phi2 (M/Ms)^alpha2] dM/Ms`, published in physical `Msun` and `Mpc^-3` at `h_obs = ShamTargetHubble`.
- **Conversion to the simulation's h** (once, at init): `log10 Ms_sim = ShamTargetLogMstar + 2 log10(h_obs / h_sim)` and `phi_i_sim = phi_i (h_sim / h_obs)^3`, with `h_sim = Hubble_h`, so stellar masses scale as `h^-2` and densities as `h^3`. For `h_sim = 0.6774`: `log10 Ms_sim = 10.68851`, `phi1_sim = 3.58870e-3`, `phi2_sim = 7.15927e-4`.
- **Cumulative density**: `n(>M) = integral_M^(120 Ms) Phi dM'`, tabulated once at init on 4096 nodes uniformly spaced in `ln M` from `10^(ShamTargetLogMassFloor - 1)` to `120 Ms_sim`; each interval is integrated in `ln M'` by Simpson's rule on its two halves and the intervals are summed from the top. At `h_sim = 0.6774`, `n(>10^8) = 3.031993e-2` and `n(>10^11.5) = 2.793662e-6 Mpc^-3`. The table must be finite and strictly decreasing; init fails otherwise.
- **Inversion**: bisection over the table's nodes, then linear interpolation of `ln M` in `ln n` (log-log). Nothing is extrapolated: a rank density outside the table fails the snapshot (only reachable for an astronomically large box). Above the published fit range (`10^11.5 Msun`) the analytic double Schechter is evaluated as written up to `120 Ms`.
- **Floor**: `ShamTargetLogMassFloor` is a physical mass at the simulation's h. Candidates whose rank density lies above the floor density are masked, never extrapolated below the fit's validity.
- **Precision**: everything is evaluated in double; `StellarMass`, `ShamVpeak` and `ShamMpeak` are stored as floats. The inversion agrees with the independent reference `_tests/sham_rank_match_reference.py` to `|delta log10 M*| <= 1e-4`.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. The module appears exactly once in `modules.pre_timestep` as `process_full_halo` and in no other FoF phase.
2. The module appears in `modules.post_snapshot` as `process_snapshot` (the registry rejects a second entry before init).
3. No output snapshot's redshift exceeds `ShamTargetRedshiftMax`.

## Properties

- Reads: `Type`, `UniqueGalaxyID`, `Mvir`, `Vmax`, `ShamVpeak`, `ShamMpeak`
- Writes: `Type` (Type 2 to Type 3), `ShamVpeak`, `ShamMpeak`, `ShamGhost`, `StellarMass`

## Parameters

All nine are required in `modules.parameters`; every value must be finite (`nan`, `inf`, `infinity` and overflowing strings such as `1e400` are rejected, as are malformed strings such as `1,0` and `abc`).

| Parameter | Units | Domain | Meaning |
|---|---|---|---|
| `ShamTargetLogMstar` | log10(Msun) at `h_obs` | finite | Schechter mass `Ms` |
| `ShamTargetPhi1` | Mpc^-3 at `h_obs` | `> 0` | First normalisation |
| `ShamTargetAlpha1` | dimensionless | finite | First faint-end slope |
| `ShamTargetPhi2` | Mpc^-3 at `h_obs` | `> 0` | Second normalisation |
| `ShamTargetAlpha2` | dimensionless | finite | Second faint-end slope |
| `ShamTargetHubble` | dimensionless | `(0, 2)` | `h_obs` of the published fit |
| `ShamTargetLogMassFloor` | log10(Msun) at `h_sim` | finite, `10^x < 120 Ms_sim` | Lowest assignable mass; denser ranks are masked |
| `ShamTargetRedshiftMax` | dimensionless | finite | Largest output-snapshot redshift the target may be applied at |
| `ShamMinVpeak` | km/s | `> 0` | Candidate completeness floor on `ShamVpeak` |

`BoxSize` and `Hubble_h` come from the simulation package and must be finite and positive, with `(Hubble_h / BoxSize)^3` a finite positive normal double. The target's slopes and normalisations must give a finite, strictly decreasing table.

## Tests

`_tests/test_unit_sham_rank_match.c` (18 cases) checks the converted target and `n(>M)` against the reference values, the inversion against the reference table, the placement guards and parameter domains, the redshift window, the FoF callback (retirement, peaks, the float bound, the reset, validation before writes), the rank match (tie order, permutation and repeat identity, the global rank across FoFs, masking, peak persistence through a masked snapshot, validation without partial assignment, empty and all-ineligible populations, non-output silence, no allocation growth) and both callbacks through the registered dispatch. `_tests/sham_rank_match_reference.py` is the independent double-precision reference (composite Simpson in `ln M` over 400,000 intervals per integral, bisection for the inverse); `_tests/test_integration_sham_rank_match.py` requires the unit test's `SHAM_DECIMAL_REFERENCE` table to equal it. Run the unit test by path with the selectors exported:

```bash
MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal tests/unit/run_tests.sh \
  models/sham/modules/sham_rank_match/_tests/test_unit_sham_rank_match.c
```

## Notes

- There is no scatter: two candidates with equal `ShamVpeak` differ in mass only through their adjacent ranks.
- Orphans are not candidates; a subhalo that loses its halo leaves the sample at the next FoF step.
- The ghost rows stay in output so halo catalogues remain complete; consumers select the sample with `ShamGhost == 0`.

## References

Conroy, Wechsler & Kravtsov (2006), ApJ 647, 201; Vale & Ostriker (2006), MNRAS 371, 1173; Reddick et al. (2013), ApJ 771, 30; Baldry et al. (2012), MNRAS 421, 621.
