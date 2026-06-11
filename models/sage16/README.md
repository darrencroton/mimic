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

## Package Contents

- `model_properties.yaml`: sage16 galaxy/model property metadata. Edit this when adding, removing, or changing model-owned galaxy properties, then run `make MODEL=sage16 generate`.
- `input/`: User-facing run parameter YAML files for mini-Millennium and full Millennium.
- `modules/`: Runtime physics modules. Each production module has its own directory containing C source, `module_info.yaml`, README, and `_tests/` where applicable.
- `modules/_tests/`: Shared cross-module tests for processing contracts and parity checks.
- `shared/`: Model-local helper headers (physics kernels, parity helpers, shared constants). These are not framework APIs; copy or reimplement them in another model package if needed. Event contracts are generated from each module's `module_info.yaml`.
- `plots/figures/`: Model-specific diagnostic plot implementations for `mimic-plot.py`.
- `plots/profiles/`: Plot profile YAML files, including mini-Millennium defaults used by the shipped run.

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
