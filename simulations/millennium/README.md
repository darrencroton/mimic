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

**Horizontal conversion (format version 3).** `converter_columns.yaml` is this package's profile for `scripts/convert/convert_trees.py --source-format lhalo_binary`. Name the file range you hold: `simulation_info.yaml` declares files 0–511, a requested file that is missing fails the conversion rather than narrowing it, and only files 0–15 have been converted and checked (23,720,119 halos). That is a labelled sample, not a whole-simulation conversion. The sampled conversion of files 0–15 runs as the version 3 package `simulations/millennium-horizontal/`: over those sixteen files, horizontal output is bitwise identical per `UniqueGalaxyID` to this package's vertical output over the same files, under `halos-only` with fixed and dynamic timesteps ([`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](../../docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md) §9–§14). Nothing is claimed for the whole simulation, which needs files 16–511, or for `sage16`. Commands, flags and what the evidence covers: [`scripts/convert/README.md`](../../scripts/convert/README.md).
