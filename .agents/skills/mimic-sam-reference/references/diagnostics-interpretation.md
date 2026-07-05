# Reading the sage16 figures scientifically

Companion to the `mimic-sam-reference` SKILL.md. This file explains what each shipped sage16 figure *shows*, what healthy output looks like, and which physics parameter moves which feature. For plot mechanics (registry, profiles, skips, invocation) see the `mimic-plots-and-analysis` skill. Figure inventory verified against `models/sage16/plots/figures/__init__.py` (18 snapshot + 4 evolution figures, 2026-07-04); observational comparison data are inline NumPy arrays inside the figure modules.

**Numbers before claims.** Never conclude "looks right" or "matches the baseline" from eyeballing these plots. A plot is a *hypothesis generator*; every scientific claim (parity, improvement, regression) needs a measured per-galaxy/per-property comparison with stated tolerances. The `mimic-scientific-method` skill owns that discipline; use these notes only to know what to measure.

## Vocabulary for reading the plots

- **Mass function**: number density of objects per unit volume per logarithmic mass bin, usually plotted as log10(Φ / Mpc^-3 dex^-1) vs log10(mass). "dex" = a factor of 10.
- **Faint end / massive end**: the low-mass slope and high-mass cutoff of a mass function.
- **Specific SFR (sSFR)**: StarFormationRate / StellarMass — growth rate per unit mass; separates star-forming (high sSFR) from quiescent (low sSFR) galaxies.

## Snapshot figures (dark-matter-only sanity first)

These four use only halo properties and must look right with *any* model, even `halos-only` — if they are wrong, the problem is upstream of the physics (reader, units, tree traversal):

| Figure | Shows | Healthy looks like |
|---|---|---|
| `halo_mass_function` | Halo number density vs Mvir | Smooth power law with exponential high-mass cutoff; turns over/noisy below ~20-particle mass (resolution, not physics) |
| `spin_distribution` | Halo spin parameter histogram | Lognormal-ish, peaking near λ ~ 0.03–0.05 |
| `velocity_distribution` | Vmax / velocity component histograms | Smooth, no spikes at sentinel values (0, −1) |
| `spatial_distribution` | Galaxy positions in the box | Cosmic web (filaments, clusters, voids); uniform random scatter means broken positions; stripes/clipping mean unit or box-wrapping errors |

## Snapshot figures (galaxy physics)

| Figure | Shows | Healthy looks like | Main knobs |
|---|---|---|---|
| `stellar_mass_function` | Number density vs StellarMass | Schechter shape: power-law faint end, knee near log10(M*/Msun) ~ 10.5–11, sharp massive-end cutoff; tracks the inline observational points | Faint end ↓ with stronger `FeedbackReheatingEpsilon`/`FeedbackEjectionEfficiency` (SN feedback); massive-end cutoff sharpens with `RadioModeEfficiency` (AGN); overall shift with `SfrEfficiency` |
| `halo_occupation` | Galaxies per halo vs halo mass | Monotonic rise; 1 galaxy at low Mvir, tens–hundreds in clusters | `ThresholdSatDisruption`, merger clock physics |
| `cold_gas_function` / `gas_mass_function` | Number density vs ColdGas | Schechter-like; massive gas disks rarer than massive stellar systems | `SfrEfficiency` (drains cold gas), cooling/AGN balance |
| `baryonic_mass_function` | Number density vs StellarMass+ColdGas | Schechter-like, slightly above the SMF | Same as SMF |
| `baryon_fraction` | Total baryons / Mvir vs Mvir | Approaches `GlobalBaryonFraction` (0.17) in clusters; strongly suppressed below ~10^11 Msun (SN ejection + reionization) | `GlobalBaryonFraction` sets the ceiling; SN parameters set the low-mass dip |
| `baryonic_tully_fisher` | Baryonic mass vs Vmax | Tight power law (slope ~4 in log-log) | Gross offsets indicate unit/h errors before physics errors |
| `specific_sfr` | sSFR vs StellarMass | Bimodal: star-forming ridge near sSFR ~ 10^-10 yr^-1 plus a quiescent cloud at high mass | AGN parameters grow the quiescent cloud; `SfrEfficiency` moves the ridge |
| `quiescent_fraction` | Fraction of low-sSFR galaxies vs StellarMass | Rises from ~0 at low mass to ~1 at high mass | `RadioModeEfficiency`, `AGNrecipe` — AGN feedback is what quenches massive galaxies |
| `black_hole_bulge_relation` | BlackHoleMass vs BulgeMass | Tight power law, M_BH ~ 10^-3 × M_bulge | `BlackHoleGrowthRate` sets normalization; scatter reflects merger history |
| `bulge_mass_fraction` | BulgeMass/StellarMass vs StellarMass | Disk-dominated at low mass → bulge-dominated at high mass | `ThresholdMajorMerger`, disk-instability chain |
| `gas_fraction` | ColdGas/(ColdGas+StellarMass) vs StellarMass | Falls with mass; dwarfs gas-rich, giants gas-poor | `SfrEfficiency`, SN reheating |
| `metallicity` | Gas metallicity vs StellarMass | Rises with mass, flattens near ~solar at high mass | `Yield` sets the normalization (metallicity ∝ Yield to first order); SN ejection sets the low-mass slope; `RecycleFraction`, `FracZleaveDisk` secondary |
| `mass_reservoir_scatter` | All reservoirs vs Mvir per galaxy | Each reservoir occupies its expected band; no negatives, no reservoir exceeding 0.17×Mvir | Everything — this is the best single-glance accounting check |

## Evolution figures (multiple snapshots)

| Figure | Shows | Healthy looks like |
|---|---|---|
| `hmf_evolution` | Halo mass function across redshifts | Massive end grows toward z=0 (hierarchical assembly); pure N-body check |
| `smf_evolution` | Stellar mass function across redshifts | Builds up over time; massive end mostly in place by z~1 |
| `sfr_density_evolution` | Cosmic SFR density vs z ("Madau plot") | Rises from high z, peaks near z ~ 2, falls ~10× to z=0 |
| `stellar_mass_density_evolution` | Integrated stellar mass density vs z | Monotonic rise; the time-integral of the SFR density (net of recycling) — if SFRD looks right but this does not, suspect the accounting, and vice versa |

The shipped run YAML's `snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16]` spans z=0 back to high redshift for mini-millennium; evolution figures need those snapshots present in the output.

## Triage: which knob moved my feature?

| Symptom in plots | First suspect |
|---|---|
| Faint end of SMF too steep (too many dwarfs) | SN feedback too weak: raise `FeedbackReheatingEpsilon` / `FeedbackEjectionEfficiency` |
| Massive end of SMF overshoots / no quiescent cloud | AGN too weak: check `AGNrecipe` ≠ 0, raise `RadioModeEfficiency` |
| Metallicity normalization uniformly high/low | `Yield` (linear, to first order) |
| Everything shifted horizontally by ~0.13 dex | An h factor applied twice or not at all — units bug, not physics (log10(0.73) ≈ −0.14) |
| Galaxies pile up at exact round values | Sentinel values leaking into analysis; check `sentinels:` in property YAML |
| Mass functions noisy/truncated at low mass | Resolution limit (~20 particles), not a bug — mask below it |
| SMF fine at z=0 but wrong at high z | Timestep/`SubSteps` sensitivity or dT handling, not calibration |

A parameter changing a plot is *evidence of coupling, not correctness*. To claim a calibration improvement, quantify: bin both runs identically, difference the binned values, and compare against the run-to-run noise floor — see the `mimic-scientific-method` skill.

## Provenance

- Figure list: `grep -A25 'SNAPSHOT_PLOTS\|EVOLUTION_PLOTS' models/sage16/plots/figures/__init__.py`
- Required properties per figure: `PLOT_REQUIREMENTS` in the same file.
- Parameter defaults quoted from `models/sage16/input/sage16_mini-millennium.yaml` (2026-07-04).
- The "healthy looks like" columns are standard galaxy-formation expectations (Croton et al. 2006, 2016 as cited in `models/sage16/README.md`), not repo-measured facts; the shipped observational overlay arrays inside each figure module are the in-repo comparison anchors.
