# Millennium Simulation Package

This package contains Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: Millennium/LHaloTree catalog fields included in the generated output schema (auto-discovered by `make generate`)
- `millennium.a_list`: 64 snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — symlink this to your local Millennium LHaloTree data
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, referenced from run files that use `mimic-plot.py`
- `_tests/input/test_simulation.yaml`: fast integration-test config that compiles against Millennium metadata while reusing the repo-local mini-Millennium L-Halo fixture

The full Millennium tree data is not downloaded automatically — symlink `snapshots/` to your local copy. The structure of a simulation package is documented in [Adding a New Simulation](../../docs/DEVELOPER-GUIDE.md#adding-a-new-simulation) in the Developer Guide.

Default integration tests use the fixture config and do not touch the full 512-file production catalog. To smoke-test locally mounted production files explicitly, build for this package and run `./mimic models/halos-only/input/halos-only_millennium.yaml` or another Millennium run file.

**Mirror maintenance:** `halo_properties.yaml`, the `.a_list` file, and the `_tests/` suites are intentional near-mirrors of the `simulations/mini-millennium/` package (simulation packages are self-contained by design). When changing any of them, apply the same change to the other package.
