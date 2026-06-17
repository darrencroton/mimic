# Consistent-Trees (Uchuu) reader validation

Status: **draft, ASCII (Phase 5a) only.** The HDF5 reader section is a placeholder
to be completed in Phase 5b. This document is the checklist for importing a real
Consistent-Trees dataset (e.g. Uchuu) and validating Mimic's ctrees readers
end-to-end once the data is available on the run machine.

## What is and isn't in the repository

**Consistent-Trees is a tree *format*, not a simulation.** Mimic ships the
*readers* (`consistent_trees_ascii`, and `consistent_trees_hdf5` after 5b) and the
format-independent topology/parsing helpers under `src/io/tree/ctrees/`. It does
**not** ship a `simulations/consistent-trees` package, because the cosmology,
particle mass, box size, file count and snapshot list are properties of a specific
*simulation* (Uchuu), not of the format. You create that package when you have the
data. sage-model (`sage-code/sage-model/io/read_tree_consistentrees_*.c`) is the
source of truth for the *format* (column layout and conventions); the readers
follow it.

## 1. Create the simulation package

Make `simulations/<name>/` (e.g. `simulations/uchuu/`) with the files below. See
[Adding a New Simulation](../DEVELOPER-GUIDE.md#adding-a-new-simulation) and the
existing `simulations/mini-millennium/` for the general package shape.

### 1a. `halo_properties.yaml` — the reader's `RawHalo` contract

The ASCII reader bridges its reconstructed `struct halo_data` into the generated
`struct RawHalo` **by field name** (see `src/io/tree/read_ctrees_ascii.c`). The
package therefore **must** declare a `RawHalo` with exactly these fields, with
ctrees-native units so the generated reference-unit accessors apply the
catalog→reference conversion (this is what fixes sage-model's missing
kpc/h→Mpc/h position step — it becomes metadata, not reader code):

| field (RawHalo)                                   | type        | core role / use         | units (native)   |
|---------------------------------------------------|-------------|-------------------------|------------------|
| `Descendant`,`FirstProgenitor`,`NextProgenitor`,`FirstHaloInFOFgroup`,`NextHaloInFOFgroup` | `int` | merger-tree links (reconstructed) | dimensionless |
| `Len`                                             | `int`       | `provides_core_role: Len` | particles (reader-derived) |
| `M_Crit200` (source `Mvir`)                       | `float`     | `provides_core_role: HaloMass` | **`Msun/h`** (accessor × 1e-10) |
| `Pos`                                             | `vec3_float`| output                  | **`Mpc/h`** (confirm vs dataset; Rockstar ASCII is Mpc/h) |
| `Vel`                                             | `vec3_float`| output                  | `km/s`           |
| `VelDisp`                                         | `float`     | output (ctrees `vrms`)  | `km/s`           |
| `Vmax`                                            | `float`     | output                  | `km/s`           |
| `Spin`                                            | `vec3_float`| output (reader-normalised) | dimensionless |
| `MostBoundID`                                     | `long long` | output (ctrees id)      | dimensionless    |
| `SnapNum`                                         | `int`       | `provides_core_role: SnapNum` | dimensionless |

Declaring `M_Crit200` as `Msun/h` (not `1e10 Msun/h`) is the crucial difference
from an L-Halo package: it makes `mimic_tree_get_HaloMass` emit `× 1e-10`. Confirm
the `Pos` units against the actual dataset packaging before trusting positions.

> The reader computes `Len = round(Mvir_native / particle_mass)` and
> `Spin = J / Mvir_native` itself (both are order-dependent on the un-scaled mass),
> so `particle_mass` in `simulation_info.yaml` must be correct.

### 1b. `simulation_info.yaml`

```yaml
input:
  first_file: 0
  last_file: <num tree files - 1>
  tree_name: tree_0_0_0.dat        # the header-bearing first tree file
  tree_type: consistent_trees_ascii
  simulation_dir: simulations/<name>/snapshots
  snapshot_list_file: simulations/<name>/<name>.a_list

simulation:
  cosmology: { omega_matter: <Uchuu>, omega_lambda: <Uchuu>, hubble_h: <Uchuu> }
  box_size:      { value: <Uchuu>, units: Mpc/h, h_convention: carried }
  particle_mass: { value: <Uchuu>, units: 1e10 Msun/h, h_convention: carried }
```

### 1c. `<name>.a_list` and `snapshots/`

- `<name>.a_list`: the dataset's snapshot scale factors, one per line, increasing.
- `snapshots/`: the ctrees ASCII output — `forests.list`, `locations.dat`, and the
  `tree_i_j_k.dat` files (point `simulation_dir` at this directory). The number of
  `tree_i_j_k.dat` files must be a perfect cube; the reader validates this from
  `locations.dat`.

## 2. Build

```bash
make MODEL=<model> SIMULATION=<name> generate
make MODEL=<model> SIMULATION=<name> -j$(nproc)        # expect 0 warnings
make MODEL=<model> SIMULATION=<name> USE-MPI=yes -j$(nproc)
```

`<model>` can be `halos-only` for a pure tree-tracking sanity pass, or `sage16`
for full physics.

## 3. ASCII reader validation checklist

- [ ] **Smoke run (serial):** a small subset completes and writes one output
      partition (`./mimic --quiet simulations/<name>/...run.yaml`). Serial uses
      `ThisTask=0`, one partition.
- [ ] **MPI run:** `mpirun -np N ./mimic ...` writes `N` output partitions
      (named by task id). Confirm each task's forest range is disjoint and the
      union covers all forests.
- [ ] **Halo-count sanity:** total halos across all output partitions equals the
      total in the dataset (sum over `locations.dat` trees, reconstructed).
- [ ] **Mass unit round-trip:** an output central's `Mvir`/halo mass equals
      `Mvir_native × 1e-10` in `1e10 Msun/h`. (If masses look ~1e10× too big, the
      package declared `1e10 Msun/h` instead of `Msun/h`.)
- [ ] **Position units:** positions in `Mpc/h` and within `[0, box_size]`. (This
      is the sage-model gap fixed by metadata — double-check against raw ctrees.)
- [ ] **Len:** `Len ≈ round(Mvir_native × 1e-10 / particle_mass)` for a sample.
- [ ] **Spin:** output `Spin ≈ J_native / Mvir_native` (dimensionless).
- [ ] **Topology spot-check:** pick one forest; confirm the FoF grouping
      (`FirstHaloInFOFgroup`/`NextHaloInFOFgroup`) and `Descendant`/
      `FirstProgenitor` chains against the raw `pid`/`upid`/`desc_id` columns.
- [ ] **Galaxy-id uniqueness:** no duplicate `UniqueGalaxyID` across tasks. The
      reader asserts the bounds it relies on (see §5); if it FATALs asking for
      more tasks, increase `-np`.
- [ ] **Scientific parity:** halo mass function / counts vs sage-model on the
      *same* dataset. Expect agreement where sage is correct; positions should
      agree with sage *after* sage's kpc/h→Mpc/h fix (Mimic does it via metadata).
- [ ] **>2000 tree files:** if the dataset has a perfect-cube file count above
      2000 (e.g. 13³ = 2197), confirm it loads — this exercises the
      `read_locations` realloc-boundary fix. (sage mis-handled exactly this.)

## 4. HDF5 reader validation — _to be completed in Phase 5b_

Placeholder. After the `consistent_trees_hdf5` reader lands, extend this section
with: the forest-HDF5 dataset layout expected, the weighted MPI forest
distribution keys (`num_simulation_tree_files`, `forest_distribution_scheme`,
`exponent_forest_dist_scheme`), `snap_num`/`snap_idx` detection, and the same
unit/topology/parity checks as §3 plus a cross-check that ASCII and HDF5 readers
produce identical galaxies for a dataset available in both forms.

## 5. Known constraints (ASCII, Phase 5a)

- **Galaxy-id bounds.** Unique ids are
  `halonr + TREE_MUL_FAC·forestnr_local + FILENR_MUL_FAC·ThisTask`. The reader
  FATALs if a task is assigned ≥ `FILENR_MUL_FAC/TREE_MUL_FAC` (1e6) forests or a
  forest has ≥ `TREE_MUL_FAC` (1e9) halos; keep `NTask` below ~9000 to avoid
  64-bit overflow of the task term. Run with enough MPI tasks for very large
  datasets.
- **One file descriptor per tree file.** The reader raises `RLIMIT_NOFILE` to the
  hard limit and holds one fd per `tree_i_j_k.dat`; extremely large file counts
  are bounded by the OS hard limit.
- **Uniform distribution only.** ASCII does not expose per-forest halo counts
  before loading, so forests are split uniformly by count. Weighted
  (halo-count-cost) distribution arrives with the HDF5 reader (5b).
- **RawHalo field-name coupling.** The reader references the `RawHalo` field names
  in §1a directly; a simulation package built with this reader must declare them.
  (Other simulation packages in the repo already do, so the shared build stays
  green.)
- **`first_file`/`last_file` are dataset metadata only.** The ASCII reader reads
  every tree file listed in `locations.dat`; it does not consult `first_file`/
  `last_file` at runtime. Output partitions and the HDF5 master file are numbered
  by MPI task rank (`0..NTask-1`), not by the file range.
