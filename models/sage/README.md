# SAGE Model Package

SAGE (Semi-Analytic Galaxy Evolution) is the main physically motivated model package shipped with Mimic. It implements a modular version of the SAGE galaxy-formation model described by Croton et al. (2006, 2016), with Mimic's physics-agnostic core handling tree traversal, memory, configuration, validation, and output while the SAGE package owns the baryonic physics.

This package is the reference example for a mature Mimic model set: it has model-local properties, runtime modules, shared helper APIs, module-owned tests, and model-specific plotting figures.

## Scientific Scope

SAGE follows baryonic reservoirs on dark-matter halo merger trees: infalling, hot, cold, ejected, stellar, bulge, black-hole, and intracluster components. The module pipeline covers reionization, gas infall, reincorporation, satellite stripping, radiative cooling, AGN heating, star formation, supernova feedback, disk instabilities, black-hole growth, starbursts, mergers, and satellite disruption.

The implementation is intended to preserve the structure and major prescriptions of SAGE while exposing them as independently testable Mimic modules. Scientific changes should be made deliberately: update the module implementation, declared dependencies, parameters, tests, and model-level run configuration together.

References:

- Croton et al. (2006), "The many lives of active galactic nuclei: cooling flows, black holes and the luminosities and colours of galaxies"
- Croton et al. (2016), "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
- White & Frenk (1991), Sutherland & Dopita (1993), Kauffmann & Haehnelt (2000), Somerville et al. (2001), Mo, Mao & White (1998), Binney & Tremaine (1987)

## Package Contents

- `model_properties.yaml`: SAGE galaxy/model property metadata. Edit this when adding, removing, or changing SAGE-owned galaxy properties, then run `make MODEL=sage generate`.
- `modules/`: Runtime SAGE physics modules. Each production module has its own directory containing C source, `module_info.yaml`, README, and `_tests/` where applicable.
- `modules/_tests/`: Shared cross-module tests for processing contracts and parity checks.
- `shared/`: SAGE-local helper headers and event contracts. These are not framework APIs; copy or reimplement them in another model package if needed.
- `plots/figures/`: SAGE-specific diagnostic plot implementations for `mimic-plot.py`.
- `plots/profiles/`: Plot profile YAML files, including Millennium defaults used by the shipped SAGE run.

## Runtime Pipeline

The shipped SAGE run configuration currently lives at `input/sage_millennium.yaml`. It builds the pipeline as:

- `pre_timestep`: reionization, infall budget preparation, disk scale setup, merger-clock initialization.
- `phase_1`: gas supply, reincorporation, satellite stripping, cooling, AGN radio-mode heating, star formation, supernova feedback, disk instability, quasar mode, and starburst feedback.
- `phase_2`: merger/disruption resolution plus event-driven quasar and starburst consumers.
- `post_timestep`: empty in the default SAGE configuration.

SAGE uses transport properties such as `InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, and `SupernovaEjectedMass` to separate calculation modules from apply/commit modules. Preserve this ordering when changing the pipeline.

## Build, Run, and Plot

```bash
make MODEL=sage
./mimic input/sage_millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=input/sage_millennium.yaml
```

Useful checks:

```bash
make MODEL=sage validate-modules
make MODEL=sage check-generated
make MODEL=sage test-unit
```

## Extending SAGE

To add or change a module:

1. Work under `models/sage/modules/<module_name>/`.
2. Declare supported processing modes, property dependencies, parameter dependencies, event contracts, tests, and docs in `module_info.yaml`.
3. Add or update properties in `model_properties.yaml` only when the model's galaxy state changes.
4. Keep reusable SAGE-only helper logic in `shared/`; do not promote it to core unless it is genuinely physics-agnostic.
5. Regenerate and validate with `make MODEL=sage generate` and `make MODEL=sage validate-modules`.

When creating a new model family from SAGE, copy the needed modules into a new `models/<model>/` package and reconcile property names, units, parameters, tests, plots, and run YAML there instead of mixing model packages at runtime.
