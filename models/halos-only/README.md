# Halos-Only Model Package

This package runs Mimic without galaxy-formation physics. Use it when you want to explore the dark-matter simulation halo catalog, validate tree loading, inspect halo inheritance, or make halo-level diagnostic plots without adding stellar, gas, metal, or black-hole properties.

`halos-only` is still a normal Mimic model package. That keeps the build, generated output schema, run provenance, tests, and plotting machinery on the same path as SAGE, SHAM, and future model packages. The difference is that this package declares no galaxy properties and ships no runtime physics modules.

## What It Produces

The output contains core and simulation/catalog halo fields generated from:

- `src/core/core_properties.yaml`
- `simulations/<simulation>/halo_properties.yaml`
- `models/halos-only/model_properties.yaml`

The halos-only model property file intentionally has an empty `galaxy_properties` list, so production outputs contain no model-owned galaxy-physics fields. The module pipeline is also empty, so Mimic performs halo tracking and output writing only.

## Package Contents

- `model_properties.yaml`: Empty model-owned galaxy-property metadata.
- `input/halos-only_mini-millennium.yaml`: User-facing mini-Millennium run file.
- `plots/figures/`: Halo/catalog diagnostic plots for `mimic-plot.py`.
- `plots/profiles/`: Plot profile defaults for halos-only runs.
- `modules/_tests/`: Utility test metadata and package-level integration checks. This directory is not a runtime physics module.

## Build, Run, and Plot

```bash
make MODEL=halos-only SIMULATION=mini-millennium
./mimic models/halos-only/input/halos-only_mini-millennium.yaml
python plot/mimic-plot/mimic-plot.py --param-file=models/halos-only/input/halos-only_mini-millennium.yaml
```

Useful checks:

```bash
make MODEL=halos-only SIMULATION=mini-millennium validate-modules
make MODEL=halos-only SIMULATION=mini-millennium check-generated
make MODEL=halos-only SIMULATION=mini-millennium tests-integration summary
```
