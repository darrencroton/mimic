# SHAM Model Package

This package contains a Mimic-native pseudo-SHAM model. It is mainly a compact proof of concept for the model-package boundary: a model-specific property file, two alternative runtime modules, and self-contained plotting diagnostics under `models/sham/`. If you are building your own model package, this is the minimal worked example of the pattern described in the [Developer Guide](../../docs/DEVELOPER-GUIDE.md#creating-physics-modules); the [sage16 package](../sage16/README.md) shows the same pattern at production scale.

## Scientific Status

This is not a complete subhalo abundance matching implementation and should not be used for precision science.

A true SHAM ranks halos or subhalos across a complete snapshot, volume, or catalogue path, then assigns galaxy properties by matching cumulative abundances. The package offers two mutually exclusive prescriptions:

- `sham_assign_stellar_mass` (the shipped default) is a deterministic local proxy that works one FoF workspace at a time. It tracks peak halo properties along each processed branch and assigns a stellar mass from an analytic stellar-to-halo mass relation with optional deterministic scatter. It cannot perform the global ranking step that defines abundance matching.
- `sham_global_rank` performs that ranking over a whole snapshot through the horizontal driver's `modules.post_snapshot` phase: it ranks every Type 0/1/2 galaxy by peak circular velocity (`ShamVpeak` descending, ties by ascending `UniqueGalaxyID`, zero-based rank `r`) and assigns the stellar mass at which the analytic cumulative function `n(>M) = n0 (M / M0)^(-alpha)` equals `(r + 0.5) / BoxSize^3`. Its target is uncalibrated: the parameters are explicit inputs, not fits to an observed stellar mass function, and it makes no observational-parity claim. Its only shipped example runs on a small synthetic fixture, which is not a complete cosmological population. See its [README](modules/sham_global_rank/README.md) for units, candidates, range failures and output fields.

Both are useful for exercising Mimic's model-set architecture, schema generation, run configuration, and plotting path.

References:

- Conroy et al. (2006), Vale & Ostriker (2006): early abundance-matching methods
- Reddick et al. (2013): scatter and subhalo proxy context
- Moster et al. (2013): double-power-law stellar-to-halo mass relation used by the default parameters
- Guo & White (2014): mini-Millennium-era galaxy-halo comparison context

## Package Contents

- `model_properties.yaml`: All SHAM-owned galaxy properties, including `ShamMpeak`, `ShamVpeak`, `ShamStellarMassNoScatter`, `ShamScatterDex`, and `ShamOrphanAge`.
- `input/`: User-facing SHAM run parameter YAML files, including the shipped mini-Millennium configuration and the `sham_global_rank` fixture example `sham_global_micro-uchuu-ascii-horizontal.yaml`.
- `modules/sham_assign_stellar_mass/`: The FoF-local module. It tracks peak proxies, assigns `StellarMass` (bulge and metal components are reset to zero), and handles Type 2 orphan ageing.
- `modules/sham_global_rank/`: The snapshot-wide rank module (`process_snapshot` in `modules.post_snapshot`, horizontal driver only). It tracks the same peak proxies and assigns `StellarMass` by global rank; it neither ages nor removes orphans. Startup rejects configuring it together with `sham_assign_stellar_mass`.
- `plots/figures/`: SHAM-specific and shared diagnostic figures for `mimic-plot.py`.
- `plots/profiles/`: Plot profile YAML files, including mini-Millennium defaults used by the shipped SHAM run.

## Runtime Pipeline

The shipped SHAM run configuration lives at `models/sham/input/sham_mini-millennium.yaml`. It runs `sham_assign_stellar_mass` as `process_full_halo` in `post_timestep`, after Mimic has advanced halo inheritance for the snapshot. `SubSteps` is set to `1` because this proxy model has no substep baryonic reservoir cycle.

The module reads `Type`, `dT`, `Mvir`, and `Vmax`; writes stellar diagnostics and SHAM proxy properties; and uses parameters prefixed with `Sham`. Type 0 and Type 1 galaxies update their peak proxies from resolved halos. Type 2 galaxies retain their last resolved peaks and accumulate orphan age until the configured maximum age is exceeded.

## Build, Run, and Plot

```bash
make MODEL=sham
./mimic models/sham/input/sham_mini-millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=models/sham/input/sham_mini-millennium.yaml
```

The global rank example needs the horizontal fixture package and runs from the repository root; its output lands in `output/sham-global-micro-uchuu-ascii-horizontal/`:

```bash
make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal
./mimic models/sham/input/sham_global_micro-uchuu-ascii-horizontal.yaml
make tests-snapshot-global-sham     # its unit and end-to-end tests on the fixture
```

Useful checks:

```bash
make MODEL=sham validate-modules
make MODEL=sham check-generated
```

## Extending SHAM

Treat this package as a starting point, not a calibrated model. `sham_global_rank` supplies a global per-snapshot ranking stage on a single process with a fully resident snapshot; it has no calibrated target, scatter, distributed reduction or measured production-scale memory. Improvements to the FoF-local module should still be framed as local proxy experiments and documented as such.

When extending the package:

1. Add SHAM-owned galaxy state to `model_properties.yaml`.
2. Declare every property and parameter dependency in the module's `module_info.yaml`.
3. Keep diagnostics under `plots/figures/` and profile defaults under `plots/profiles/`.
4. Regenerate and validate with `make MODEL=sham generate` and `make MODEL=sham validate-modules`.
