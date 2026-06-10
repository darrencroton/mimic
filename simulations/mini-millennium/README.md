# mini-Millennium Simulation Package

This package contains mini-Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: mini-Millennium/LHaloTree catalog fields included in the generated output schema (auto-discovered by `make generate`)
- `mini-millennium.a_list`: 64 snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — mini-Millennium files land here on `first_run.sh`, or replace with a symlink to your local data
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, referenced from run files that use `mimic-plot.py`
- `input_data_manifest.yaml`: lists the required data files; used by `first_run.sh` to know what to download

This is the simulation package used by the shipped quick-start configuration — see the [User Guide](../../docs/USER-GUIDE.md) for running it. To create a package like this for your own simulation, see [Adding a New Simulation](../../docs/DEVELOPER-GUIDE.md#adding-a-new-simulation) in the Developer Guide.
