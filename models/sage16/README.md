# sage16 Model Package

**sage16** is a complete, physically motivated model package for Mimic, and the current build default: a modular port of SAGE (Semi-Analytic Galaxy Evolution) as calibrated and published in Croton et al. (2016), with the 2006 physics lineage behind it. Mimic's physics-agnostic core handles tree traversal, memory, configuration, validation, and output, while this package owns the baryonic physics.

This package is also the reference example for a mature Mimic model set: it has model-local properties, runtime modules, shared helper APIs, module-owned tests, and model-specific plotting figures. For the general model-package concepts, see the [Developer Guide](../../docs/DEVELOPER-GUIDE.md); for running and configuring this model, see the [User Guide](../../docs/USER-GUIDE.md).

## Scientific Scope

SAGE follows baryonic reservoirs on dark-matter halo merger trees: infalling, hot, cold, ejected, stellar, bulge, black-hole, and intracluster components. The module pipeline covers reionization, gas infall, reincorporation, satellite stripping, radiative cooling, AGN heating, star formation, supernova feedback, disk instabilities, black-hole growth, starbursts, mergers, and satellite disruption.

The implementation preserves the structure and prescriptions of SAGE while exposing them as independently testable Mimic modules. Scientific changes should be made deliberately: update the module implementation, declared dependencies, parameters, tests, and model-level run configuration together — and be aware that any physics change moves the package away from its validated parity baseline.

References:

- Croton et al. (2006), "The many lives of active galactic nuclei: cooling flows, black holes and the luminosities and colours of galaxies"
- Croton et al. (2016), "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
- White & Frenk (1991), Sutherland & Dopita (1993), Kauffmann & Haehnelt (2000), Somerville et al. (2001), Mo, Mao & White (1998), Binney & Tremaine (1987)

## Agreement with Original SAGE

Measured on mini-Millennium against the original SAGE code (`sage-home/sage-model`, `master` at `b8424cf`), both run serially with identical parameters on the same trees, at the eight output snapshots of `sage16_mini-millennium.yaml`:

- Halo properties (`Mvir`, `Rvir`, `Vvir`, `Vmax`, `VelDisp`, positions, velocities, spins) are bit-identical for every galaxy, and galaxies match one to one at every snapshot, except one z = 0 satellite that SAGE has and `sage16` does not (36,531 against 36,530).
- Baryonic properties agree per galaxy to about one part in 10⁶ for most galaxies, not bit for bit: at z = 0, 83–99.9% of matched galaxies are within 1e-6 relative per reservoir and size property (`HotGas` lowest at 83%), and the 99th-percentile relative difference reaches up to 7.5e-3. The two codes differ in ways that compound, chiefly that Mimic carries core and halo properties in double precision where SAGE uses float.
- The scientific outcomes agree: z = 0 global reservoir totals within 0.2%, and the stellar, cold-gas and black-hole mass functions within 1% in every 0.25 dex bin holding at least 20 galaxies.

`tools/compare_sage_mimic.py` reproduces the comparison from the two codes' HDF5 output.

## Package Contents

- `model_properties.yaml`: sage16 galaxy/model property metadata. Edit this when adding, removing, or changing model-owned galaxy properties, then run `make MODEL=sage16 generate`.
- `input/`: User-facing run parameter YAML files for all supported simulations: mini-Millennium, full Millennium, micro-Uchuu (L-Halo binary, HDF5 forests, ASCII), mini-Uchuu, and full Uchuu.
- `modules/`: Runtime physics modules. Each production module has its own directory containing C source, `module_info.yaml`, README, and `_tests/` where applicable.
- `modules/_tests/`: Shared cross-module tests for processing contracts and parity checks.
- `shared/`: Model-local helper headers (physics kernels, parity helpers, shared constants). These are not framework APIs; copy or reimplement them in another model package if needed. Event contracts are generated from each module's `module_info.yaml`.
- `plots/figures/`: Model-specific diagnostic plot implementations for `mimic-plot.py`.
- `plots/profiles/`: Plot profile YAML files, including mini-Millennium defaults used by the shipped run.
- `tools/compare_sage_mimic.py`: Galaxy-by-galaxy comparison of `sage16` HDF5 output against original SAGE HDF5 output (matching, per-property bit-identical and within-tolerance fractions, relative-difference quantiles, JSON report).

## Runtime Pipeline

The shipped run configuration lives at `models/sage16/input/sage16_mini-millennium.yaml`. It builds the pipeline as:

- `pre_timestep`: reionization, infall budget preparation, disk scale setup, merger-clock initialization.
- `galaxy_physics`: gas supply, reincorporation, satellite stripping, cooling, AGN radio-mode heating, star formation, supernova feedback, disk instability, quasar mode, and starburst feedback.
- `satellite_mergers`: merger/disruption resolution plus event-driven quasar and starburst consumers.
- `post_timestep`: empty in the default configuration.

SAGE uses transport properties such as `InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, and `SupernovaEjectedMass` to separate calculation modules from apply/commit modules. Preserve this ordering when changing the pipeline.

## Build, Run, and Plot

sage16 is the default model, so plain `make` builds it; the explicit selectors below behave identically:

```bash
make MODEL=sage16
./mimic models/sage16/input/sage16_mini-millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml
```

Useful checks:

```bash
make MODEL=sage16 validate-modules
make MODEL=sage16 check-generated
make MODEL=sage16 tests-unit
```

## Extending sage16

To add or change a module:

1. Work under `models/sage16/modules/<module_name>/`.
2. Declare supported processing modes, property dependencies, parameter dependencies, event contracts, tests, and docs in `module_info.yaml`.
3. Add or update properties in `model_properties.yaml` only when the model's galaxy state changes.
4. Keep reusable model-local helper logic in `shared/`; do not promote it to core unless it is genuinely physics-agnostic.
5. Regenerate and validate with `make MODEL=sage16 generate` and `make MODEL=sage16 validate-modules`.

When creating a new model family from sage16, copy the needed modules into a new `models/<model>/` package and reconcile property names, units, parameters, tests, plots, and run YAML there instead of mixing model packages at runtime. See [Creating Physics Modules](../../docs/DEVELOPER-GUIDE.md#creating-physics-modules) for the full workflow.
