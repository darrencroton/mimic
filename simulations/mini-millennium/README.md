# mini-Millennium Simulation Package

This package contains mini-Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: mini-Millennium/LHaloTree catalog fields included in the generated output schema (auto-discovered by `make generate`)
- `mini-millennium.a_list`: 64 snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — mini-Millennium files land here on `first_run.sh`, or replace with a symlink to your local data
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, referenced from run files that use `mimic-plot.py`
- `tests/data/test_simulation.yaml`: shared repo-local fast fixture config used by default integration tests

This is the simulation package used by the shipped quick-start configuration — see the [User Guide](../../docs/USER-GUIDE.md) for running it. To create a package like this for your own simulation, see [Adding a New Simulation](../../docs/DEVELOPER-GUIDE.md#adding-a-new-simulation) in the Developer Guide.

**Mirror maintenance:** `halo_properties.yaml`, the `.a_list` file, and the `_tests/` suites are intentional near-mirrors of the `simulations/millennium/` package (simulation packages are self-contained by design). When changing any of them, apply the same change to the other package.
