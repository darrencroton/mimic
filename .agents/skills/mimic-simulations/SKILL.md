---
name: mimic-simulations
description: Working with Mimic simulation packages — understanding, configuring, extending, or debugging simulation metadata, halo properties, and tree readers. Load when any simulation package work is in scope.
---

# Mimic Simulations

Full reference: `docs/DEVELOPER-GUIDE.md` (simulation packages section) and `simulations/<SIM>/README.md`. This skill covers the structure and critical patterns.

## Simulation Package Structure

```
simulations/<sim>/
  simulation_info.yaml         # cosmology, box size, input paths, tree type
  halo_properties.yaml         # on-disk halo catalog field definitions (in file order)
  <sim>.a_list                 # snapshot scale factors, one per line
  plot_profile.yaml            # optional: simulation-specific plot profile (model-local profiles take precedence)
  _tests/integration/          # optional: reader behaviour tests
  README.md
  snapshots -> /path/to/data   # symlink to actual tree data
```

Current simulation packages include `mini-millennium`, `millennium`, `micro-uchuu`, `micro-uchuu-hdf5`, `micro-uchuu-ascii`, `mini-uchuu`, and `uchuu`.

## simulation_info.yaml

```yaml
input:
  first_file: 0
  last_file: 7
  tree_name: trees_063
  tree_type: lhalo_binary       # selects the on-disk tree reader
  simulation_dir: simulations/mini-millennium/snapshots
  snapshot_list_file: simulations/mini-millennium/mini-millennium.a_list

simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size:
    value: 62.5
    units: Mpc/h
    h_convention: carried
  particle_mass:
    value: 0.0860657
    units: 1e10 Msun/h
    h_convention: carried
```

`tree_type` selects the reader format. Current registered readers are `lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, and `consistent_trees_hdf5`; HDF5 readers require an HDF5-enabled build. Do not conflate `tree_type` with `processing_order` — they are orthogonal.

## halo_properties.yaml

Defines the on-disk catalog record in **file order** — the order of entries must match the byte layout of the binary tree file. The generator uses this to produce `struct RawHalo` and the HDF5 reader.

```yaml
halo_properties:
  - name: Descendant
    type: int
    units: dimensionless
    description: "Index of descendant halo, or -1"
    provides_core_role: Descendant    # binds to the core-required accessor

  - name: Mvir_200c
    type: float
    units: 1e10 Msun/h
    h_convention: carried
    description: "Virial mass"
    provides_core_role: HaloMass      # binds this field to the HaloMass core role

  - name: AuxField
    type: float
    units: km/s
    description: "Auxiliary field not used by core"
    notes: "Read to preserve catalog layout; not consumed by pipeline"
    # no provides_core_role, no output, no init
```

Required core roles that must be bound (via `provides_core_role`): `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`, `SnapNum`, `Len`, `HaloMass`.

## Key Schema Notes

- `provides_core_role` decouples catalog column names from the names the core expects — rename a column freely by adding/changing this field.
- `h_convention`: `carried` (value in units including h), `free` (physical units), `none` (dimensionless).
- `source`: use when the on-disk column name differs from the desired property name.
- Fields with no `output`, `provides_core_role`, or `init` are read to preserve byte layout; add a `notes` line explaining this.

## Snapshot List

`<sim>.a_list`: one scale factor per line, in time order (earliest first). The reader maps snapshot indices to these scale factors.

## simulation.config Override

A run file can point to an alternate simulation metadata file while keeping the compiled SIMULATION package:

```yaml
simulation:
  name: mini-millennium
  config: path/to/alternate_simulation_info.yaml
```

This is useful for test fixtures that need non-default input paths without a full new simulation package.

## Regenerating After Changes

```bash
# Default package pair
make generate

# Non-default simulation
make MODEL=sage16 SIMULATION=my-new-sim generate
make MODEL=sage16 SIMULATION=my-new-sim validate-modules
make MODEL=sage16 SIMULATION=my-new-sim
```

Always use the same `MODEL`/`SIMULATION` pair for generate, validate-modules, tests, and build.

## Tree Reader Selection

`input.tree_type` in `simulation_info.yaml` selects the reader. Adding support for a new tree format requires a new reader in `src/io/tree/` — see `docs/DEVELOPER-GUIDE.md` for the `TreeReader` vtable contract.

`consistent_trees_hdf5` is the high-throughput forests-HDF5 path for Uchuu-style catalogues. It caches task-range `ForestInfo`, keeps per-file `Forests/<field>` handles open during the partition, validates field schema at cache-open time, and uses a fixed `CTREES_READ_WINDOW_BYTES` read window (`128 MiB` per rank) for normal forests. Forests larger than the window use the cached-handle direct read path. Treat the window as an internal bounded-memory constant, not simulation metadata or a run-YAML parameter.
