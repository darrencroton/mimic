# mini-Millennium Simulation Package

This package contains mini-Millennium-specific catalog metadata used by Mimic:

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: mini-Millennium/LHaloTree catalog fields included in the generated output schema (auto-discovered by `make generate`)
- `mini-millennium.a_list`: 64 snapshot scale factors used for redshift and timestep calculations
- `snapshots/`: tree data directory — mini-Millennium files land here on `first_run.sh`, or replace with a symlink to your local data
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, referenced from run files that use `mimic-plot.py`
- Integration tests use the cross-package `tests/data/test_simulation.yaml` as the shared fast fixture; unlike the `simulations/millennium/` package, mini-Millennium has no simulation-local `_tests/input/` override

This is the simulation package used by the shipped quick-start configuration — see the [User Guide](../../docs/USER-GUIDE.md) for running it. To create a package like this for your own simulation, see [Adding a New Simulation](../../docs/DEVELOPER-GUIDE.md#adding-a-new-simulation) in the Developer Guide.

**Mirror maintenance:** `halo_properties.yaml`, the `.a_list` file, and the `_tests/` suites are intentional near-mirrors of the `simulations/millennium/` package (simulation packages are self-contained by design). When changing any of them, apply the same change to the other package.

**Horizontal conversion (format version 3).** `converter_columns.yaml` is this package's profile for `scripts/convert/convert_trees.py --source-format lhalo_binary` over files 0–7. The whole local dataset has been converted and checked halo by halo against this package's own vertical reader: 1,533,122 halos, including 29,291 descendant links that skip a snapshot, every one preserved as a gap. Mass stays float32 in `1e10 Msun/h`, as the source stores it. The converted dataset runs as the version 3 package `simulations/mini-millennium-horizontal/`, and it is Mimic's reference gapped route: on the complete real data, horizontal output is bitwise identical per `UniqueGalaxyID` to this package's vertical output under `halos-only` and `sage16`, with fixed and dynamic timesteps ([`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](../../docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md) §1–§8). Commands, flags and what the evidence covers: [`scripts/convert/README.md`](../../scripts/convert/README.md).
