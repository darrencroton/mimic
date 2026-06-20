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
*readers* (`consistent_trees_ascii` and `consistent_trees_hdf5`) and the
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
- [ ] **ASCII vs HDF5 cross-check:** for a dataset available in both forms, target
      a snapshot **before z=0** (e.g. snap48) for the identity check. At snap49
      (z=0) the ASCII reader applies `fix_flybys` (see §5) and will produce ~55K
      fewer Type 0 halos than HDF5; this is expected behaviour, not a reader bug.
      At all earlier snapshots both readers bridge to the identical `RawHalo`
      contract and must produce byte-identical halo counts, masses, positions, and
      topology.

## 5. Known constraints

### fix_flybys: z=0 FoF topology differs between ASCII and HDF5/L-Halo

The Consistent-Trees ASCII reader calls `fix_flybys()` (`src/io/tree/ctrees/ctrees_utils.c:318`) as part of per-forest topology reconstruction, immediately before `fix_upid` and `assign_mergertree_indices`. A ctrees *forest* groups together merger trees that are linked by flyby interactions — halos that passed within each other's virial radii at some point without merging. At z=0 (the maximum snapshot in each forest), such a forest can contain several independent FoF groups (`pid == -1` in the ctrees data). `fix_flybys` picks the most massive of these and demotes all others to satellites by:

1. Setting their `pid` to the most massive z=0 FoF halo's id, wiring them into that halo's FoF group via `FirstHaloInFOFgroup`/`NextHaloInFOFgroup`.
2. Negating their `MostBoundID` as an inline flyby marker (this propagates through `bridge_halo_data_to_rawhalo` into the output HDF5).

This behaviour was ported from sage-model's ctrees reader and is intentional for SAGE-family galaxy physics, where each forest is expected to map to a single central galaxy at z=0. It is confined strictly to the maximum snapshot: progenitor chains at all earlier snapshots are unaffected and produce identical output across all three formats.

**Observed numbers for micro-Uchuu (halos-only, snap49):**

| Format | Type 0 | Type 1 | Type 2 | Total |
|--------|--------|--------|--------|-------|
| ctrees ASCII | 440,651 | 116,657 | 629,026 | 1,186,334 |
| L-Halo binary | 496,374 | 61,295 | 629,026 | 1,186,695 |
| ctrees HDF5 | expected ≈ lhalo | | | |

The 55,362 halos that are Type 1 in ASCII but Type 0 in lhalo/hdf5 are the flyby FoF groups. They span the full mass range (0.065–23,880 × 10¹⁰ M☉/h) and represent ~10–25% of what would be Type 0 in each mass bin. The halo mass function plot (Type 0 only) is therefore systematically lower in ASCII at snap49 relative to the other two formats. A further 361 halos present in lhalo at snap49 are absent from ASCII at snap49; most appear at earlier ASCII output snapshots or non-output snapshots.

The ctrees HDF5 reader reads `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` directly from pre-stored uchuutools columns and does not call `fix_flybys`, so its z=0 output is expected to agree with L-Halo (the §4c cross-check should confirm this once the data is available).

**Model-specific impacts:**

- `UniqueGalaxyID` and `UniqueCentralGalaxyID` are not affected (they are computed from halo index and partition number, not `MostBoundID`).
- sage16 modules do not use `MostBoundID`; physics is unchanged for those modules. Flyby halos at z=0 receive one timestep of satellite physics (Type 1 code path) in ASCII. Their full progenitor histories at earlier snapshots are unaffected.
- The SHAM model seeds its RNG from `MostBoundID`. Negated values for flyby halos produce different stellar mass draws; SHAM outputs for those halos will differ between ASCII and lhalo/hdf5.

**Cross-format comparison guidance:** snap49 total halo counts (all types) or any snapshot before snap49 are safe to compare across all three formats. Restrict Type 0 / halo mass function comparisons to pre-z=0 snapshots when checking ASCII against hdf5 or lhalo.

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

## 6. micro-Uchuu on NT: dataset survey and compatibility assessment

The files below are on `dcroton@nt.swin.edu.au`. Access is via key-based SSH. All oz214 datasets represent the **genuine micro-Uchuu cosmology** (Planck 2015: Ω_M=0.3089, Ω_Λ=0.6911, h=0.6774, box=100 Mpc/h, m_p=0.0325×10¹⁰ M☉/h, 50 snapshots from a=0.06688 to a=0.99951). The oz004 files use a different cosmology and must not be used as micro-Uchuu.

The name `microuchuu` in par files is a symlink: `/fred/oz214/simulations/uchuu/microuchuu → U100`.

### 6a. Dataset identity — the cosmology trap

The oz004 tree file (`/fred/oz004/msinha/simulations/uchuu_suite/U100/trees/tree_0_0_0.dat`) has a header reading `Omega_M = 0.307115; h0 = 0.677700` and the companion par file refers to "Uchuu100_MDPL2". These do not match Uchuu's published Planck 2015 cosmology. The oz004 dataset predates the oz214 one, has the same file sizes and forest/halo counts, and appears to be a precursor or mis-labelled variant. **Do not use it for micro-Uchuu work.**

All five oz214 datasets below are the **same underlying trees** (440,651 forests, 22,580,924 halos, 561,266 trees) packaged in different formats. Halo counts are cross-verified to agree across every format.

### 6b. Available formats and compatibility

| # | Location (on NT, under `/fred/oz214/simulations/uchuu/U100/`) | Format | Size | Mimic tree_type | Compatible? |
|---|---|---|---|---|---|
| 1 | `mergertrees/MicroUchuu_mergertree_info.h5` + `MicroUchuu_mergertree.h5` | uchuutools forests-HDF5 | 1.2KB + 13GB | `consistent_trees_hdf5` | **Yes** |
| 2 | `mergertrees/MicroUchuu_mergertree_released.h5` | old flat uchuutools HDF5 | 3.5GB | — | **No** — missing `Nfiles`/`File<n>` structure |
| 3 | `mergertrees/ascii_trees/MicroUchuu.trees` (+ index files) | Consistent-Trees ASCII | 11GB | `consistent_trees_ascii` | **Yes** |
| 4 | `mergertrees/ascii_trees/forest.h5` + `forest_0-3.h5` | uchuutools forests-HDF5, 4-file | 1.4KB + 4×3.1GB | `consistent_trees_hdf5` | **Yes** |
| 5 | `lhalo-binary-mergertree/Uchuu100_Planck_lhalo_binary.{0..3}` | L-Halo binary | 4×~570MB | `lhalo_binary` | **Yes** (converted from #3) |

**Not compatible (#2):** `MicroUchuu_mergertree_released.h5` has Uchuu-correct cosmology and the same data but uses the old flat uchuutools layout (all data at `/Forests/`, no `/Nfiles` root attribute, no `File<n>` groups). Mimic's `consistent_trees_hdf5` reader expects the `File<n>` group structure. Reading it would require a new reader variant; it can be ignored in favour of #1.

**Derived dataset (#5):** The lhalo-binary files were converted from the ASCII trees (#3) using sage-model. They carry the same halos, confirmed by matching total count (22,580,924). The par file left in that directory (`consistent_trees_hdf5`) is a run artefact and does not describe the binary files.

**4-file set (#4) vs single pair (#1):** Both are uchuutools forests-HDF5 with identical total counts. #4 was built from the local ASCII trees (#3); #1 was built directly from the original trees on the production machine. #1 is the more authoritative source. Use #4 if you want to exercise multi-file MPI splitting with `first_file`/`last_file`.

### 6c. Key HDF5 structural notes (for format #1 and #4)

- **Info/data split.** The 1.2KB info file (`MicroUchuu_mergertree_info.h5`) is an HDF5 container whose `File0/Forests/*` datasets are virtual/external references into the companion 13GB data file (`MicroUchuu_mergertree.h5`). Both files must be co-located; h5py follows the links transparently. Point `tree_name` at the info file only.
- **`File<n>` attributes present:** `Nforests`, `Nhalos`, `contiguous-halo-props=True`, plus all metadata attributes required by the reader.
- **`simulation_params` group** is present under `File0/` with `Omega_M`, `Omega_L`, `hubble`, `Boxsize`. The cosmology guard check will compare these against the simulation package values.
- **Snapshot field:** `Snap_num` (int64) — the older Consistent-Trees field name, not `Snap_idx`. The reader auto-detects this correctly.
- **ForestInfo** compound dtype: `ForestID`, `ForestHalosOffset`, `ForestNhalos`, `ForestNtrees` (all int64). `ForestNhalos` is present and enables weighted MPI forest distribution.
- **4-file variant (#4):** `forest.h5` declares `Nfiles=4`. Each `File<n>` group in the info file links to `forest_<n>.h5` in the same directory. Set `first_file=0`, `last_file=3`.

### 6d. Simulation packages — created

Three simulation packages covering formats #1, #3, and #5 have been created in the repository. Each is self-contained with `simulation_info.yaml`, `halo_properties.yaml`, `micro-uchuu.a_list`, `plot_profile.yaml`, `README.md`, `snapshots.txt` (NT path and `ln -s` command), and `_tests/integration/test_reader_smoke.py` (skips when data is unavailable):

| Package directory | Format | tree_type |
|---|---|---|
| `simulations/micro-uchuu-hdf5/` | uchuutools forests-HDF5 (format #1) | `consistent_trees_hdf5` |
| `simulations/micro-uchuu-ascii/` | Consistent-Trees ASCII (format #3) | `consistent_trees_ascii` |
| `simulations/micro-uchuu-lhalo/` | L-Halo binary (format #5) | `lhalo_binary` |

To activate a package, create the `snapshots/` symlink (see each package's `snapshots.txt`) and build:

```bash
# HDF5 reader
ln -s /fred/oz214/simulations/uchuu/U100/mergertrees simulations/micro-uchuu-hdf5/snapshots
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 -j$(nproc)
./mimic models/halos-only/input/halos-only_micro-uchuu-hdf5.yaml

# ASCII reader
ln -s /fred/oz214/simulations/uchuu/U100/mergertrees/ascii_trees simulations/micro-uchuu-ascii/snapshots
make MODEL=halos-only SIMULATION=micro-uchuu-ascii -j$(nproc)
./mimic models/halos-only/input/halos-only_micro-uchuu-ascii.yaml

# L-Halo binary reader
ln -s /fred/oz214/simulations/uchuu/U100/lhalo-binary-mergertree simulations/micro-uchuu-lhalo/snapshots
make MODEL=halos-only SIMULATION=micro-uchuu-lhalo -j$(nproc)
./mimic models/halos-only/input/halos-only_micro-uchuu-lhalo.yaml
```

**Key `halo_properties.yaml` difference:** The ctrees packages (`micro-uchuu-hdf5`, `micro-uchuu-ascii`) declare `M_Crit200` with `units: Msun/h` (native ctrees units). The lhalo package uses `units: 1e10 Msun/h` (same as mini-Millennium). This means the generated reference-unit accessor for ctrees applies the `× 1e-10` conversion automatically, while the lhalo package stores the pre-converted value directly.

Format #4 (4-file HDF5) is not yet a separate package. To test it, point a copy of `micro-uchuu-hdf5/simulation_info.yaml` at `ascii_trees/forest.h5` with `first_file: 0`, `last_file: 3`.

### 6e. Scale factor list

The authoritative scale factor list for all micro-Uchuu formats is at `/fred/oz214/simulations/uchuu/U100/Uchuu100_scalefactor.txt` (50 lines, a=0.06688 to a=0.99951, one value per line). This matches snapshot numbers 1–50 embedded in the tree files (`Snap_num` field runs 49 at z=0 down to 0 at the earliest snapshot, so the list index and snap number are offset by 1).

The oz004 file `u400_planck2016_50.a_list` uses different scale factor values and must not be substituted.

### 6f. `halo_properties.yaml` field mapping reminder

When creating the `simulations/micro-uchuu/halo_properties.yaml`, see §1a for the required `RawHalo` fields. For micro-Uchuu:

- `M_Crit200` sources from `Mvir` in native Msun/h → declare units `Msun/h` so the reference-unit accessor applies `× 1e-10`.
- `Pos` is in **Mpc/h** (confirmed from ASCII header `X/Y/Z: Halo position (Mpc/h comoving)`; applies to all three formats since they derive from the same source).
- `Spin` is the scalar spin parameter stored in the `Spin` field (not a J-vector); the reader-normalised `Spin = J/Mvir_native` convention in §1a applies to J-vector fields — verify which the ASCII reader path uses when bridging `Spin` to `RawHalo`.
- The `Len` field is reader-computed: `round(Mvir_native / particle_mass)` where `particle_mass = 0.0325 × 10¹⁰ M☉/h` (from `simulation_info.yaml`).

---

## 7. Integration tests

### 7a. Per-package smoke tests (created, skip until data available)

Each simulation package ships `_tests/integration/test_reader_smoke.py`. These tests use the framework `TestSkipped` protocol and skip cleanly when:

- The compiled `SIMULATION` doesn't match the package (wrong build).
- `snapshots/` symlink is absent (data not on this machine).
- The Mimic executable is not built.

When data is available, the smoke tests run a halos-only z=0 pass and assert:
- Mimic exits 0.
- Output contains >100,000 halos (sanity floor for the full micro-Uchuu dataset).

Run them:
```bash
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 tests-integration
make MODEL=halos-only SIMULATION=micro-uchuu-ascii tests-integration
make MODEL=halos-only SIMULATION=micro-uchuu-lhalo tests-integration
```

### 7b. Deferred: cross-format parity test (`tests/integration/test_ctrees.py`)

There is no automated cross-reader integration test yet. The readers are exercised by the unit tests on their format-independent helpers (`tests/unit/test_ctrees_support.c`) and end-to-end by the checklists in §3/§4c against a real dataset.

**To do once the simulation packages are running on NT:**

- [ ] Add `tests/integration/test_ctrees.py` asserting the per-format checklist items (halo count, mass round-trip `Mvir × 1e-10`, positions within `[0, box_size]`, `Len`/`Spin` conventions, galaxy-id uniqueness) automatically — the §3/§4c checks, automated.
- [ ] Assert ASCII (`micro-uchuu-ascii`) and HDF5 (`micro-uchuu-hdf5`) output identical galaxies: same total halo count, identical `MostBoundID` sets at z=0, mass/position agreement within float precision. This is the §4c cross-check.
- [ ] Emit structured `MIMIC_RESULT:` markers so the test participates in `make tests-integration summary`.
- [ ] Gate the test on data availability with `TestSkipped` (same pattern as the per-package smoke tests).
