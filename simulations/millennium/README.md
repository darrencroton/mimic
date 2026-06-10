# Millennium Simulation Package

This package contains Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: Millennium/LHaloTree catalog fields included in the generated output schema (auto-discovered by `make generate`)
- `millennium.a_list`: 64 snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — symlink this to your local Millennium LHaloTree data
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, referenced from run files that use `mimic-plot.py`
- `input_data_manifest.yaml`: lists the required data files (512 tree files, trees_063.0 through trees_063.511)

The full Millennium tree data is not downloaded automatically — symlink `snapshots/` to your local copy. The structure of a simulation package is documented in [Adding a New Simulation](../../docs/DEVELOPER-GUIDE.md#adding-a-new-simulation) in the Developer Guide.
