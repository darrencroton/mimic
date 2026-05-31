# Millennium Simulation Package

This package contains Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: Millennium/LHaloTree catalog fields included in the generated output schema
- `millennium.a_list`: snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — mini-Millennium files land here on `first_run.sh`, or replace with a symlink to your local data
- `plot_profile.yaml`: simulation-specific plotting defaults
