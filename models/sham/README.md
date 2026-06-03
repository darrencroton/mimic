# SHAM Model Package

This package contains a Mimic-native pseudo-SHAM model. It is mainly a compact proof of concept for the model-package boundary: a model-specific property file, one runtime module, and self-contained plotting diagnostics under `models/sham/`.

## Scientific Status

This is not a complete subhalo abundance matching implementation and should not be used for precision science.

A true SHAM ranks halos or subhalos across a complete snapshot, volume, or catalogue path, then assigns galaxy properties by matching cumulative abundances. Mimic currently processes one FoF workspace at a time, so this package cannot yet perform the global ranking step that defines abundance matching.

The current model is a deterministic local proxy. It tracks peak halo properties along each processed branch and assigns a stellar mass from an analytic stellar-to-halo mass relation with optional deterministic scatter. It is useful for exercising Mimic's model-set architecture, schema generation, run configuration, and plotting path.

References:

- Conroy et al. (2006), Vale & Ostriker (2006): early abundance-matching methods
- Reddick et al. (2013): scatter and subhalo proxy context
- Moster et al. (2013): double-power-law stellar-to-halo mass relation used by the default parameters
- Guo & White (2014): Millennium-era galaxy-halo comparison context

## Package Contents

- `model_properties.yaml`: All SHAM-owned galaxy properties, including `ShamMpeak`, `ShamVpeak`, `ShamStellarMassNoScatter`, `ShamScatterDex`, and `ShamOrphanAge`.
- `modules/sham_assign_stellar_mass/`: The only runtime module. It tracks peak proxies, assigns `StellarMass`, mirrors that mass into `BulgeMass` for simple diagnostics, and handles Type 2 orphan ageing.
- `plots/figures/`: SHAM-specific and shared diagnostic figures for `mimic-plot.py`.
- `plots/profiles/`: Plot profile YAML files, including Millennium defaults used by the shipped SHAM run.

## Runtime Pipeline

The shipped SHAM run configuration currently lives at `input/sham_millennium.yaml`. It runs `sham_assign_stellar_mass` as `process_full_halo` in `post_timestep`, after Mimic has advanced halo inheritance for the snapshot. `SubSteps` is set to `1` because this proxy model has no substep baryonic reservoir cycle.

The module reads `Type`, `dT`, `Mvir`, and `Vmax`; writes stellar diagnostics and SHAM proxy properties; and uses parameters prefixed with `Sham`. Type 0 and Type 1 galaxies update their peak proxies from resolved halos. Type 2 galaxies retain their last resolved peaks and accumulate orphan age until the configured maximum age is exceeded.

## Build, Run, and Plot

```bash
make MODEL=sham
./mimic input/sham_millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=input/sham_millennium.yaml
```

Useful checks:

```bash
make MODEL=sham validate-modules
make MODEL=sham check-generated
```

## Extending SHAM

Treat this package as a starting point, not a calibrated model. The most important missing capability is a global catalogue-ranking stage. Until Mimic has that stage, improvements should be framed as local proxy experiments and documented as such.

When extending the package:

1. Add SHAM-owned galaxy state to `model_properties.yaml`.
2. Declare every property and parameter dependency in the module's `module_info.yaml`.
3. Keep diagnostics under `plots/figures/` and profile defaults under `plots/profiles/`.
4. Regenerate and validate with `make MODEL=sham generate` and `make MODEL=sham validate-modules`.
