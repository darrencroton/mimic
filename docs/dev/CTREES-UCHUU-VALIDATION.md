# Consistent-Trees (Uchuu) reader validation

Status: **ASCII (Phase 5a) and forests-HDF5 (Phase 5b).** This document is the
checklist for importing a real Consistent-Trees dataset (e.g. Uchuu) and
validating Mimic's ctrees readers end-to-end once the data is available on the
run machine. The ASCII reader (`consistent_trees_ascii`) reads the Rockstar
`forests.list` + `locations.dat` + `tree_i_j_k.dat` output; the HDF5 reader
(`consistent_trees_hdf5`, HDF5 builds only) reads the forests-HDF5 packaging
produced by uchuutools.

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
  tree_name: tree_0_0_0.dat        # ASCII: the header-bearing first tree file
  tree_type: consistent_trees_ascii
  simulation_dir: simulations/<name>/snapshots
  snapshot_list_file: simulations/<name>/<name>.a_list
  # Optional, ctrees only — forest -> MPI-task load balancing. ASCII always
  # splits uniformly; the HDF5 reader honours these (see §4):
  forest_distribution_scheme: uniform      # uniform|linear|quadratic|exponent|generic_power
  exponent_forest_dist_scheme: 0.7         # only used by exponent/generic_power

simulation:
  cosmology: { omega_matter: <Uchuu>, omega_lambda: <Uchuu>, hubble_h: <Uchuu> }
  box_size:      { value: <Uchuu>, units: Mpc/h, h_convention: carried }
  particle_mass: { value: <Uchuu>, units: 1e10 Msun/h, h_convention: carried }
```

For the **HDF5** reader use `tree_type: consistent_trees_hdf5` and set
`tree_name` + the package's tree-file extension so that
`simulation_dir/tree_name<ext>` resolves to the forests-HDF5 metadata/data file
(the reader derives the extension from the registered reader, currently `.h5`).
`first_file`/`last_file` select the inclusive range of `File<n>` groups to
process; they must lie within the dataset's `/Nfiles`.

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

## 4. HDF5 reader validation (forests-HDF5)

The `consistent_trees_hdf5` reader (`src/io/tree/read_ctrees_hdf5.c`, HDF5 builds
only) reads the forests-HDF5 packaging written by uchuutools. Unlike the ASCII
reader, the merger-tree pointers are already in the file, so there is **no
topology reconstruction** — the reader maps a per-task forest range onto the
input files, reads each forest's contiguous halo slab, applies the shared value
conventions (spin/Len on native `Mvir`), and bridges into `RawHalo`.

### 4a. Expected dataset layout

A single forests-HDF5 file (`simulation_dir/tree_name<ext>`) with:

- root attributes `/Nfiles` (`int64`) and `/TotNforests` (`int64`);
- a group `File<n>` per file `n` carrying attributes `Nforests` (`int64`),
  `contiguous-halo-props` (`int8`, must be `1` — array-of-structs is not
  supported), and a `simulation_params` group with `Omega_M`, `Omega_L`,
  `hubble`, `Boxsize` (all `double`) — these are checked against the simulation
  package (`numpy.allclose` tolerances: abs `1e-8`, rel `1e-5`) and a mismatch is
  fatal;
- a compound dataset `File<n>/ForestInfo` with at least `ForestNhalos` (`int64`,
  read for weighted distribution) and the per-forest `foresthalosoffset` /
  `forestnhalos` slab descriptors;
- a group `File<n>/Forests` with one dataset per halo field:
  `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`,
  `NextHaloInFOFgroup` (`int64`); `Mvir`, `x`, `y`, `z`, `vrms`, `vmax`, `vx`,
  `vy`, `vz`, `Jx`, `Jy`, `Jz` (`double`); `id` (`int64`); and the snapshot
  field as **`Snap_num`** (older ctrees, integer) or **`Snap_idx`** (newer/Uchuu,
  integer — or `double` if the converter mis-typed it). The reader auto-detects
  the snapshot field name and on-disk type.

Only the fields the `RawHalo` bridge consumes are read; `M200b`/`M200c` are not
read because the bridge does not carry them.

### 4b. Weighted MPI forest distribution

The forests-HDF5 metadata exposes per-forest halo counts up front, so the HDF5
reader can balance MPI load by **cost** rather than forest count. Configure it in
the package `input:` section (§1b):

- `forest_distribution_scheme`: `uniform` (default), `linear` (cost = nhalos),
  `quadratic` (nhalos²), `exponent` (nhalos^⌊exp⌋), or `generic_power`
  (pow(nhalos, exp)). `uniform` skips reading `ForestNhalos` entirely.
- `exponent_forest_dist_scheme`: the exponent for the two power schemes (default
  `0.7`; ignored otherwise).

The ASCII reader ignores these and always splits uniformly (it cannot know
per-forest halo counts before loading). Galaxy-id bounds are identical to ASCII
(see §5): `unit` = task-local forest index, `output_id` = `ThisTask`, full
`FILENR_MUL_FAC` stride.

### 4c. Validation checklist

- [ ] **Smoke run (serial):** `tree_type: consistent_trees_hdf5` completes and
      writes one output partition (`ThisTask=0`).
- [ ] **MPI run + scheme sweep:** `mpirun -np N` writes `N` partitions; rerun
      with `forest_distribution_scheme: linear` and confirm the per-task forest
      ranges shift toward equal halo counts while the union still covers all
      forests exactly once.
- [ ] **Cosmology guard:** corrupt one `simulation_params` value and confirm the
      reader aborts with the file-vs-package mismatch message.
- [ ] **Snapshot field:** confirm `Snap_num`/`Snap_idx` auto-detection picks the
      present field and the right integer/double type (`SnapNum` looks sane).
- [ ] **Same unit/topology checks as §3:** halo-count sanity, mass round-trip
      (`Mvir × 1e-10`), positions in `[0, box_size]` (declare `Pos` units to
      match the dataset — Uchuu HDF5 may store kpc/h, unlike Rockstar ASCII's
      Mpc/h), `Len`, `Spin = J/Mvir`, galaxy-id uniqueness.
- [ ] **ASCII vs HDF5 cross-check:** for a dataset available in both forms, the
      two readers must produce the same galaxies (same halo counts, masses,
      positions, and topology), since both bridge to the identical `RawHalo`
      contract via the shared conventions.

## 5. Known constraints

- **Galaxy-id bounds (both readers).** Unique ids are
  `halonr + TREE_MUL_FAC·forestnr_local + FILENR_MUL_FAC·ThisTask`. Each reader
  FATALs if a task is assigned ≥ `FILENR_MUL_FAC/TREE_MUL_FAC` (1e6) forests or a
  forest has ≥ `TREE_MUL_FAC` (1e9) halos; keep `NTask` below ~9000 to avoid
  64-bit overflow of the task term. Run with enough MPI tasks for very large
  datasets.
- **One file descriptor per tree file (ASCII).** The ASCII reader raises
  `RLIMIT_NOFILE` to the hard limit and holds one fd per `tree_i_j_k.dat`;
  extremely large file counts are bounded by the OS hard limit. (The HDF5 reader
  holds one open `File<n>` group per file in its task's range instead.)
- **Distribution scheme.** ASCII does not expose per-forest halo counts before
  loading, so it always splits uniformly by forest count. The HDF5 reader honours
  `forest_distribution_scheme` / `exponent_forest_dist_scheme` (§4b) and can
  weight by halo-count cost.
- **Contiguous (SOA) layout only (HDF5).** The HDF5 reader requires
  `contiguous-halo-props == 1`; the array-of-structs packaging is not supported
  (it aborts), matching sage-model.
- **RawHalo field-name coupling.** The reader references the `RawHalo` field names
  in §1a directly; a simulation package built with this reader must declare them.
  (Other simulation packages in the repo already do, so the shared build stays
  green.)
- **`first_file`/`last_file` are dataset metadata only.** The ASCII reader reads
  every tree file listed in `locations.dat`; it does not consult `first_file`/
  `last_file` at runtime. Output partitions and the HDF5 master file are numbered
  by MPI task rank (`0..NTask-1`), not by the file range.
