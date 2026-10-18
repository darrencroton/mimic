# SAGE26 Reader Brief — the cut Shin-Uchuu L-Halo dataset

**Purpose:** everything the SAGE26 repository needs to read the cut Shin-Uchuu merger trees that Mimic's `write_vertical_trees.py` writes for the `shin-uchuu-lhalo` package, and to compare SAGE26's galaxies with Mimic's. It is written for whoever implements the reader in SAGE26. It is a brief, not an implementation: nothing here was built or run in SAGE26, and no claim below about SAGE26 goes beyond what its source states at the commit pinned next.

**Owner:** the Mimic project; written under [`MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md`](MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md) (Slice 8, Decision 9). Archive it when the SAGE26 work is done or dropped.

---

## The SAGE26 source this brief read

- Repository `https://github.com/sage-home/SAGE26`, commit **`e1174dda14838ec4b8e30f1452212e8d907d6a58`** (committed 2026-10-08 13:49 +1100). On 2026-10-18, `git ls-remote` showed this commit was still the remote `HEAD`, so the brief reads the current SAGE26.
- The clone was read only, at `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical-rehearsal/sage26-clone` (clean working tree). Nothing in it was built or run.
- Every `src/...:line` citation in this brief is a path and line in that clone at that commit. Citations of Mimic files name the Mimic repository path.

**Struct sizes.** Where the brief gives a SAGE26 struct size, it was worked out by hand from the declaration, assuming natural alignment on an LP64 host (`int`/`float` 4 bytes, `long long`/`double`/`uint64_t` 8 bytes). It was not measured with `sizeof` in SAGE26, because nothing there was compiled. A Python `ctypes` replica of the two declarations agrees: `struct GALAXY` 2,184 B and `struct halo_data` 104 B.

---

## 1. The dataset as SAGE26 must see it

### Files

- **Names.** `<dir>/shin-uchuu-cut.<k>` for `k` from 0 to the last file, with no extension. Mimic's reader opens `<simulation_dir>/<tree_name>.<filenr>` (`src/io/vertical/binary.c`, `.file_extension = ""` at `:231`). SAGE26 forms `"%s/%s.%d%s"` from `SimulationDir`, `TreeName`, the file number and `TreeExtension` (`src/io/read_tree_lhalo_binary.c:62-65`). `TreeExtension` is empty for every non-HDF5 tree type (`src/core_read_parameter_file.c:431-449`), so `TreeName shin-uchuu-cut` gives the right names.
- **Range.** The production plan, made with the default caps, has **361 files, 0 to 360**: 22,503,649,037 halos in 244,953,607 forests (`MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md`, "The production file plan"). Use every file. The forests are numbered across files, so a missing file shifts every later forest's global number (section 4).
- **Endianness.** Every value is little-endian, as the converter writes it (the record's dtype is `<i4`/`<f4`/`<i8`, `convert/mimic-convert/vertical/record.py`). SAGE26 reads raw bytes into host-order structs (`src/io/read_tree_lhalo_binary.c:393`), so it must run on a little-endian host or swap the bytes.

### The header

The header is the classic L-Halo header, identical to the one SAGE26 already reads:

| Bytes | Type | Content |
|---|---|---|
| 0–3 | int32 | `ntrees`: forests in this file |
| 4–7 | int32 | `totNHalos`: halos in this file |
| 8 … 8 + 4·ntrees − 1 | int32[ntrees] | halos in each forest, in file order |
| then | records | every forest's records back to back, in file order |

Mimic reads the header at `src/io/vertical/binary.c:101-125` and the forests at `:205-217`. SAGE26 reads the two counts at `src/io/read_tree_lhalo_binary.c:146-152` and the per-forest counts from byte 8 (`:265-275`, `:494`). Both counts fit `int32` with room to spare: the largest file holds 126,380,326 halos, and no file holds more than 1,000,000 forests.

### The 88-byte record

There is one record per halo, 88 bytes long. The layout is the compiled `struct RawHalo` that Mimic generates from `simulations/shin-uchuu-lhalo/halo_properties.yaml`. `record.record_layout(record.record_dtype(...))` derives it, and `simulations/shin-uchuu-lhalo/_tests/unit/test_record_layout.c` pins it. The layout digest is `ab04dcbae52ffd11b1e773fe3c9aafecc22371f4583eeadf2353930e5bdf5e64`.

| Offset | Size | Field | Type | Unit and meaning |
|---|---|---|---|---|
| 0 | 4 | `Descendant` | int32 | forest-local index of the descendant; −1 if none |
| 4 | 4 | `FirstProgenitor` | int32 | forest-local index of the main progenitor; −1 if none |
| 8 | 4 | `NextProgenitor` | int32 | forest-local index of the next progenitor of the same descendant; −1 if none |
| 12 | 4 | `FirstHaloInFOFgroup` | int32 | forest-local index of the FoF central; **itself for a central**; never −1 |
| 16 | 4 | `NextHaloInFOFgroup` | int32 | forest-local index of the next FoF member; −1 if last |
| 20 | 4 | `Len` | int32 | particles, **the stored value** `round(Mvir × 1e-10 / particle_mass)`, not a raw particle count |
| 24 | 4 | `M_Crit200` | float32 | **Msun/h** (not 1e10 Msun/h): the Rockstar/Consistent-Trees `Mvir` (M200c) |
| 28 | 12 | `Pos[3]` | float32 | Mpc/h, comoving |
| 40 | 12 | `Vel[3]` | float32 | km/s, peculiar |
| 52 | 12 | `Spin[3]` | float32 | (Mpc/h)(km/s): **already specific angular momentum J/Mvir** (a zero-mass halo carries raw J) |
| 64 | 4 | `VelDisp` | float32 | km/s |
| 68 | 4 | `Vmax` | float32 | km/s |
| 72 | 8 | `MostBoundID` | int64 | Consistent-Trees halo `id`, strictly positive, unique across the catalogue |
| 80 | 4 | `SnapNum` | int32 | snapshot number, 0 to 69 |
| 84 | 4 | (padding) | — | alignment padding to 88; contents carry no meaning |

Sources for the units and meanings: `simulations/shin-uchuu-lhalo/halo_properties.yaml:1-17` (the record and its conventions), `:61-72` (`Len`, `M_Crit200`), `:96-99` (`Spin`), `:128-131` (`MostBoundID`); and `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:92` (`Spin` normalisation) and `:95` (`MostBoundID` always positive). The values are the horizontal version 3 dataset's own, copied with no change of unit, type or convention. The fields that differ from SAGE's record are the mass unit, the absent fields, and the order after `Vel` (`Spin` before `VelDisp` and `Vmax`).

### Order and links

- **Forest order.** Files hold contiguous ranges of `ForestIndex` in file-number order. Within a file, forests ascend in `ForestIndex`. So a forest's global number is `first_forest` of its file (from `file_plan.json`) plus its position in the file.
- **Within-forest order.** A forest's records are in `HaloRankInForest` order, the "reference vertical-driver order": descending snapshot first, then FoF-group runs with the central first, then the progenitor-chain insertion order (`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:97`; `convert/mimic-convert/rewriter/__init__.py:34-35`). A halo's index in its forest is its rank. The converter refuses any forest whose ranks are not exactly 0..n−1 (`convert/mimic-convert/vertical/assemble.py:17-18`).
- **Links.** All five links are forest-local `int32` indices into the forest's own records. Only −1 means "none". `FirstHaloInFOFgroup` is never −1 and points to the halo itself for a central. FoF links stay within one snapshot. The converter verifies every link as it writes it (`convert/mimic-convert/vertical/assemble.py:29-31`; `HORIZONTAL-HDF5-FORMAT.md:316-336`). This is the L-Halo link convention SAGE26 already walks (`src/core_build_model.c:94-160`; `src/model_misc.c:69-70` asserts that a new galaxy's halo is its own `FirstHaloInFOFgroup`).

### The file cut and the identity multiplier

- **Cut rule.** A new file starts when adding the next forest would take the file above **100,000,000 halos** or **1,000,000 forests**. A forest larger than the halo cap sits in a file of its own: file 0 holds the 126,380,326-halo super-forest (`ForestIndex` 0) alone. The caps are converter options, with these defaults (`convert/mimic-convert/vertical/plan.py:52-53`). The hard refusals are a file above `INT32_MAX` halos or forests (`:56`).
- **Identity multiplier.** Mimic's `UniqueGalaxyID` for this dataset uses the multiplier **2 × 10¹⁰** (`simulations/shin-uchuu-lhalo/simulation_info.yaml`, `unique_galaxy_id_multiplier: 20000000000`). It is the same multiplier as the horizontal `shin-uchuu` package, so the two packages' ids agree. SAGE26 does not use this number. It matters only for joining output (section 4).

### Do not use SAGE26's `lhalo_binary` reader unchanged

SAGE26's `lhalo_binary` reader takes the header correctly and then misreads everything after it:

- It casts every record to `struct halo_data` (`src/core_simulation.h:17-46`), which is **104 bytes** by the declaration: five `int` links, `int Len`, `float M_Mean200, Mvir, M_TopHat, Pos[3], Vel[3], VelDisp, Vmax, Spin[3]`, `long long MostBoundID` at offset 80, and `int SnapNum, FileNr, SubhaloIndex`, `float SubHalfMass`.
- It computes every forest's byte offset as counts × `sizeof(struct halo_data)` (`src/io/read_tree_lhalo_binary.c:278-281`, `:296`) and reads `nhalos × sizeof(struct halo_data)` bytes per forest (`:393`). Every forest after a file's first one therefore starts at the wrong offset, and every record after the first in a forest is misaligned by 16 bytes per halo.
- The first record is misread too. Offset 24, which holds `M_Crit200`, would be read as `M_Mean200`; `Pos[0]` would be read as `Mvir`; and so on. `MostBoundID` and `SnapNum` sit at different offsets in the two layouts.
- The last forest of each file asks for 16 × (halos in the file) bytes beyond the end of the file. `mypread` aborts on a zero-byte read (`src/core_utils.c:187-201`).

Even if the sizes happened to agree, the masses would be off by 10¹⁰. **Add the reader in section 2. Do not try to read these files as `lhalo_binary`.**

---

## 2. The reader to add

### A new `TreeType`

Add a value, for example `mimic_lhalo_binary`, in four places:

- the `Valid_TreeTypes` enum, before `num_tree_types` (`src/core_allvars.h:33-47`);
- the parameter-file name and enum tables, which a compile-time check keeps the same length as the enum (`src/core_read_parameter_file.c:470-480`);
- the three dispatch switches: setup (`src/core_io_tree.c:63-97`), cleanup (`:131-171`) and load (`:186-226`).

It needs no HDF5, so it belongs outside the `#ifdef HDF5` blocks, beside `lhalo_binary`.

The simplest route is to copy `src/io/read_tree_lhalo_binary.c`, changing it in two ways:

1. **Record size.** Replace `sizeof(struct halo_data)` with the on-disk record size, 88, in the byte-offset arithmetic (`:280`, `:296`) and the read (`:393`).
2. **Load into SAGE's struct.** Read each forest's `nhalos × 88` bytes into a temporary buffer, then fill a `struct halo_data` array field by field from the table below.

Keep the rest of `setup_forests_io_lht_binary` unchanged: the header pass, the forest distribution, the per-file `FileNr` and `original_treenr` bookkeeping (`:298-309`), and the GalaxyIndex multipliers (`:330-333`, section 3). A compile-time check that the record table's offsets end at 88 guards the layout. For a run-time check, the layout digest above is what Mimic's converter records in each conversion's `manifest.json` (`record.digest`).

### The record table, and the mass scale

| `struct halo_data` field | Value |
|---|---|
| `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup` | from offsets 0, 4, 8, 12, 16, unchanged |
| `Len` | from offset 20, unchanged |
| `Mvir` (the union with `M200c`, `src/core_simulation.h:29-32`) | `M_Crit200` from offset 24 × 1e-10: Msun/h to SAGE26's 10¹⁰ Msun/h. Compute it in double and store float32, as SAGE26's Consistent-Trees readers do (`src/io/read_tree_consistentrees_hdf5.c:627`) |
| `Pos[3]`, `Vel[3]` | from offsets 28 and 40, unchanged |
| `Spin[3]` | from offset 52, **unchanged**. It is already J/Mvir. Do not divide by the mass, which the Consistent-Trees readers do because their input is raw J (`src/io/read_tree_consistentrees_hdf5.c:622-625`) |
| `VelDisp`, `Vmax` | from offsets 64 and 68, unchanged |
| `MostBoundID` | from offset 72, unchanged. Keep it bit-exact: it is the origin of every galaxy's own `MostBoundID` field (set from the halo record at creation, `src/model_misc.c:99`, and refreshed from the host's record only for Type 0 and 1 galaxies of the first occupied progenitor, `src/core_build_model.c:240`, `:252-254`; see section 4). SAGE26 keys its FFB draw on that galaxy field and the snapshot (`src/model_regimes.c:143-150`, `:225`), and it writes the host halo's record value out as `SimulationHaloIndex` (`src/io/save_gals_hdf5.c:878`) |
| `SnapNum` | from offset 80, unchanged |

### The fields the dataset does not have

SAGE26's own source shows the model never reads any of these five fields. Outside the readers that fill them, nothing under `src/` names `M_Mean200`, `M_TopHat`, `SubhaloIndex` or `SubHalfMass`. The galaxy's file number comes from `forests_info->FileNr`, not from the halo's `FileNr` field: the reader comments that the field "can't guarantee … is correct" (`src/io/read_tree_lhalo_binary.c:298-300`). So the values below are sentinels or bookkeeping, chosen to follow SAGE26's own readers. None of them changes a galaxy.

| Field | Value | Why |
|---|---|---|
| `M_Mean200` | `-1.0f` | The dataset has no M200b. A negative sentinel cannot be mistaken for a mass. Copying `Mvir` would label an M200c value as M200b |
| `M_TopHat` | `-1.0f` | The dataset has no top-hat mass, for the same reason. (SAGE26's Consistent-Trees HDF5 reader puts M200c here, `src/io/read_tree_consistentrees_hdf5.c:586`. In this dataset M200c is already `Mvir`.) |
| `FileNr` | the file number being read | It is known exactly, and the reader already records it per forest (`src/io/read_tree_lhalo_binary.c:300`). The Consistent-Trees readers use −1 only because they have no such number (`src/io/read_tree_consistentrees_hdf5.c:635`) |
| `SubhaloIndex` | `-1` | The dataset has no subhalo index into a group catalogue. −1 is the convention of SAGE26's HDF5 readers (`src/io/read_tree_consistentrees_hdf5.c:636`; `src/io/read_tree_lhalo_hdf5.c:486`; `src/io/read_tree_genesis_hdf5.c:972`) |
| `SubHalfMass` | `-1.0f` | Not in the dataset. The same readers set `-1.0f` (`src/io/read_tree_consistentrees_hdf5.c:637`; `src/io/read_tree_lhalo_hdf5.c:487`) |

### Forest distribution

**Keep SAGE26's default**, `ForestDistributionScheme generic_power_in_nhalos` with `ExponentForestDistributionScheme 0.7` (`src/core_read_parameter_file.c:88`, `:129`). Each MPI task then gets one contiguous global forest range, with the cost of each forest weighted as nhalos^0.7 (`src/io/forest_utils.c:111-117`, `:141-200`).

Two consequences at this scale:

- Any scheme other than `uniform_in_forests` makes every task read every file's count table into one `int64` array of all forests (`src/io/read_tree_lhalo_binary.c:162-170`, `:455-503`). That is 244,953,607 × 8 B ≈ **1.96 GB per task**, freed before the forests are processed (`:182-184`).
- `uniform_in_forests` avoids that table, but splits by forest count alone (`src/io/forest_utils.c:33-75`). With forests from 1 to 126,380,326 halos, the tasks would be badly unbalanced.

Whatever the scheme, the super-forest is one forest and runs on one task (section 3).

### Parameter-file keys

These are SAGE26's required and optional keys (`src/core_read_parameter_file.c:141-168`). The values are the dataset's, from `simulations/shin-uchuu-lhalo/simulation_info.yaml` and the producer's own SAGE parameter file recorded in `MIMIC-DEVELOPMENT-PATHWAY.md` (the NT holdings table: `PartMass 0.0000897`, `BoxSize 140.0`, `Omega 0.3089`, `OmegaLambda 0.6911`, `Hubble_h 0.6774`, `LastSnapShotNr 69`).

| Key | Production | Subset (section 5) |
|---|---|---|
| `TreeType` | the new value, e.g. `mimic_lhalo_binary` | same |
| `TreeName` | `shin-uchuu-cut` | `shin-uchuu-cut` |
| `SimulationDir` | the directory holding the files | the subset conversion's directory |
| `FirstFile` / `LastFile` | `0` / `360` | `0` / `32` |
| `NumSimulationTreeFiles` | `361` | `33` |
| `FileWithSnapList` | `simulations/shin-uchuu-lhalo/shin-uchuu.a_list` (70 scale factors, snapshots 0 to 69) | same |
| `LastSnapshotNr` | `69` | `69` |
| `Hubble_h`, `Omega`, `OmegaLambda` | `0.6774`, `0.3089`, `0.6911` | same |
| `BoxSize` | `140.0` (Mpc/h) | same |
| `PartMass` | `0.0000897` (10¹⁰ Msun/h; 8.97 × 10⁵ Msun/h) | same |
| `UnitLength_in_cm`, `UnitMass_in_g`, `UnitVelocity_in_cm_per_s` | `3.08568e+24`, `1.989e+43`, `100000` (Mpc/h, 10¹⁰ Msun/h, km/s, as SAGE26's `input/millennium.par:106-108`) | same |
| `BaryonFrac` and the physics keys | SAGE26's science choice, not a property of the dataset | — |
| `ForestDistributionScheme`, `ExponentForestDistributionScheme` | `generic_power_in_nhalos`, `0.7` (the defaults) | same |

**`NumSimulationTreeFiles` must equal the number of files.** SAGE26 computes each task's `frac_volume_processed` as the sum, over its files, of (its forests in the file / forests in the file), divided by `NumSimulationTreeFiles`. It assumes every file spans the same volume (`src/io/read_tree_lhalo_binary.c:314-324`), and it refuses a fraction outside [0, 1] (`src/core_io_tree.c:113-117`). These files are `ForestIndex` ranges, not equal volumes. So over all tasks the fractions sum to 1 only for a complete run of every file. A partial file range reports a volume fraction that is not the true one. Do not use it to normalise number densities.

---

## 3. SAGE26's own limits at this scale

### The galaxy-index scheme, and the caps that keep it valid

SAGE26 forms `GalaxyIndex = GalaxyNr + forestnr × ForestNr_Mulfac + filenr × FileNr_Mulfac` (`src/core_save.c:227-312`). For L-Halo binary the multipliers are `FileNr_Mulfac = 10¹⁵` and `ForestNr_Mulfac = 10⁹` (`src/io/read_tree_lhalo_binary.c:330-333`). `forestnr` is the forest's position in its own file (`original_treenr`, `:305`; passed in at `src/core_save.c:158-159`). The scheme refuses a run in two cases (`src/core_save.c:252`):

- `GalaxyNr > 10⁹`. `GalaxyNr` counts galaxies created within one forest (`src/model_misc.c:95-96`; `src/sage.c:347`). A forest's creations cannot exceed its halos, and the largest forest has 126,380,326 halos, so this holds.
- forest position × 10⁹ > 10¹⁵, that is, a forest position in a file above 10⁶. **The converter's forest cap of 1,000,000 per file keeps every position at most 999,999.** The cap was chosen for this reason (the plan's Decision 4). A conversion with a larger `--max-forests-per-file` would break SAGE26's ids.

The file number times 10¹⁵ stays far inside 64 bits: 360 × 10¹⁵ = 3.6 × 10¹⁷.

### Per-forest `GALAXY` allocation, and the super-forest

For every forest, SAGE26 allocates its halos (`nhalos × sizeof(struct halo_data)`, 104 B, `src/io/read_tree_lhalo_binary.c:367`) and `nhalos` `halo_aux_data` (20 B by the declaration, `src/core_allvars.h:227-234`; `src/sage.c:334`). It also allocates **two** `struct GALAXY` arrays, `HaloGal` and `Gal`, each of `maxgals = MAXGALFAC × nhalos` elements, with `MAXGALFAC` 1 and a floor of 10,000 (`src/sage.c:331-336`; `src/macros.h:19`). Both grow by 10,000 elements at a time when full (`src/core_build_model.c:215-219`, `:653-657`).

`struct GALAXY` (`src/core_allvars.h:106-222`) is about **2,184 bytes** by hand count. That is the struct-size assumption stated at the top, with `STEPS` 10 and `ABSOLUTEMAXSNAPS` 200 (`src/macros.h:17`, `:20`). The two star-formation-history arrays alone are 2 × 200 × 4 = 1,600 B (`src/core_allvars.h:170-171`).

**Estimate (labelled as such: not measured, nothing was run).** For the 126,380,326-halo super-forest in file 0:

| Allocation | Bytes |
|---|---|
| `HaloGal` + `Gal`, 2 × 126,380,326 × 2,184 | ≈ 552.0 GB |
| halos, 126,380,326 × 104 | ≈ 13.1 GB |
| `HaloAux`, 126,380,326 × 20 | ≈ 2.5 GB |
| **Requested at the forest's start** | **≈ 568 GB** |

That is more than the 512 GiB (≈ 550 GB) of the Mac Studio this dataset was built on. How much of it becomes resident depends on how many `HaloGal` and `Gal` elements the forest actually writes, which this brief cannot estimate from the source alone. If the forest's surviving galaxy records approach its halo count, `HaloGal` alone is ≈ 276 GB resident. `Gal` holds one FoF group's galaxies at a time. The next four largest forests (71,150,152; 55,080,340; 51,040,732 and 49,139,058 halos) request about 320, 247, 229 and 221 GB on the same arithmetic (4,492 B per halo).

The proposed reader's temporary per-forest buffer (section 2: nhalos × 88 B, about 11.1 GB for the super-forest) comes on top of the ≈ 568 GB if it is still live when the `GALAXY` arrays are allocated. Freeing it as soon as the forest's `struct halo_data` array is filled avoids that.

**The super-forest is SAGE26's own memory question.** It decides whether, and on which host, the production run is possible. Measure it on the largest subset forest first (section 5). Shrinking `MAXGALFAC` or allocating on demand would be a SAGE26 change, outside this brief.

### int32 forest-local indices

SAGE26 holds every halo link as `int` (`src/core_simulation.h:20-24`), walks forests with `int halonr` (`src/sage.c:356`; `src/core_build_model.c:94-98`), and keeps galaxy counts and indices as `int`/`int32_t` (`src/sage.c:331`, `:344`, `:347`; `src/core_allvars.h:113-115`). Its counts table is `int32` on disk, promoted in memory (`src/io/read_tree_lhalo_binary.c:265-275`). The largest forest-local index is 126,380,325, about 17 times below `INT32_MAX`. The dataset's link type is `int32` too, and the converter refuses a rank the type cannot hold (`convert/mimic-convert/vertical/assemble.py:212`, `:284-287`).

---

## 4. SAGE26's output and Mimic's identity

### The two id schemes never coincide

**Mimic.** For a galaxy born in a tree halo, `UniqueGalaxyID = creation_halonr + 2 × 10¹⁰ × (forestnr_global + 1)` (`src/core/core_properties.yaml:81-84`; `src/include/galaxy_id.h:53-56`).

- `creation_halonr` is the forest-local index (= rank) of the halo where the galaxy was created. It persists for the galaxy's whole life.
- `forestnr_global` is the `ForestIndex`. For this dataset, that is the file plan's `first_forest` for the file plus the forest's position in the file.

Mimic's output does not carry the current host halo's index. `HaloNr` is internal, not output (`src/core/core_properties.yaml:73-77`).

**SAGE26.** `GalaxyIndex = GalaxyNr + forest-in-file × 10⁹ + file × 10¹⁵` (section 3). `GalaxyNr` is a creation counter within the forest (`src/model_misc.c:95-96`), not a halo index, and the multipliers differ. So **a SAGE26 `GalaxyIndex` never equals a Mimic `UniqueGalaxyID`**, and neither can be computed from the other.

### What SAGE26's output does give

SAGE26 writes these per galaxy (`src/io/save_gals_hdf5.c:873-879`; the same in `src/io/save_gals_binary.c:315-317`):

- **`SAGETreeIndex`** = `original_treenr`, the forest's position within its file (`src/io/read_tree_lhalo_binary.c:305`). **Mimic's forest number is the file plan's prefix for that file plus `SAGETreeIndex`**: `ForestIndex = file_plan.files[file].first_forest + SAGETreeIndex`. The file is not written per galaxy. Take it from the output file or task, or recover it as `GalaxyIndex ÷ 10¹⁵`.
- **`SAGEHaloIndex`** = `g->HaloNr`, the galaxy's **current host halo's** within-forest index. SAGE26 sets it when the galaxy is created (`src/model_misc.c:98`) and **overwrites it** each time the galaxy is carried into a descendant halo (`src/core_build_model.c:234-235`). SAGE26 reads the records in file order with the same forest-local indices, so `SAGEHaloIndex` equals that halo's `HaloRankInForest`.
- **`SimulationHaloIndex`** = `llabs(MostBoundID)` of the current host halo, which is the Consistent-Trees halo id.

So (file, `SAGETreeIndex`, `SAGEHaloIndex`) identifies **the host halo at an output snapshot**: a correspondence between host halos in the dataset. It is not the persistent birth-halo component of Mimic's `UniqueGalaxyID`. A central's host halo is in general not the halo it was born in, and a satellite's host can be another FoF member. The same holds for SAGE26's in-memory `GALAXY.MostBoundID`, which is not written out. It is set from the birth halo at creation (`src/model_misc.c:99`). It is then updated to the host halo's id only for Type 0 and Type 1 galaxies (`src/core_build_model.c:240`), and only for the galaxies of the first occupied progenitor (`if(prog == first_occupied)`, `:252-254`). A Type 2 orphan keeps its last value.

### A galaxy-identity join

A galaxy-for-galaxy join needs **a birth or lineage mapping stated for both codes**:

- **Mimic** gives the birth halo directly: decode `UniqueGalaxyID` into (`ForestIndex`, `creation_halonr`). Use the shared multiplier 2 × 10¹⁰. A strictly negative id is a created record, not a tree halo (`src/core/core_properties.yaml:84`), and has no birth halo to join on.
- **SAGE26** keeps no birth halo in its output. The natural addition on the SAGE26 side is one output field: the forest-local index of the halo where `init_galaxy` created the galaxy (`src/core_build_model.c:347-350`; `src/model_misc.c:65-99`), carried unchanged through `galaxies[ngal] = halogal[...]` (`src/core_build_model.c:234`). That is SAGE26's own decision.

With such a field, (`ForestIndex`, birth rank) is a key both codes state. **`MostBoundID` is the data that ties either side to the halo catalogue.** It is in the record at offset 72, unique and positive, so any (forest, rank) pair becomes a Consistent-Trees halo id. SAGE26 already writes its host halo's id as `SimulationHaloIndex`.

Without a birth mapping, the honest comparison is between host halos at an output snapshot: (file, `SAGETreeIndex`, `SAGEHaloIndex`) on the SAGE26 side, against the same halo's galaxies on the Mimic side, found through their (`ForestIndex`, rank) via `UniqueCentralGalaxyID` for centrals. It is a halo-level match, not a galaxy identity.

### What a SAGE26 against `sage16` comparison can claim

- **`sage16` is near-bit-parity with SAGE 2016, not with SAGE26.** Its parity campaign against the original SAGE gave ≥ 98% bit-identical per property, with a one-galaxy z = 0 count difference understood (`.agents/skills/mimic-failure-archaeology/SKILL.md:57`). SAGE26 carries physics `sage16` does not have, among them CGM and hot-halo regimes, feedback-free bursts, and H₂ and H I reservoirs (`src/core_allvars.h:110-111`, `:144-146`), and an FFB draw keyed on each galaxy's `MostBoundID` field and the snapshot (`src/model_regimes.c:143-150`, `:225`).
- So the comparison is **scientific, not an identity**: mass functions, scaling relations and population statistics on the same trees, and the halo-level match above. The same-trees property *is* exact. With the reader in section 2, SAGE26 and Mimic read identical topology and identical halo values, apart from the mass scale and SAGE26's float32 rounding of `Mvir` × 1e-10. Differences in galaxies are therefore the models', not the inputs'.
- It cannot claim that any galaxy "is the same galaxy" without the birth mapping above. It cannot claim agreement to a tolerance as evidence of correctness for either code.

---

## 5. What SAGE26 should run first

**The Stage R cut subset conversion, before the production dataset.** It was recorded in `MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md` ("Rehearsal", "The subset conversions"):

- **33 files**, `shin-uchuu-cut.0` to `shin-uchuu-cut.32`, made with `--max-halos-per-file 12500000` and the default forest cap. In all, **406,668,896 halos in 7,206,195 forests**; the largest forest has 1,250,835 halos (`ForestIndex` 4,310,475).
- The plan is `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical-rehearsal/conv/subset-w1/file_plan.json` (md5 `0b2d1af9183ab871931f8a0f823cb1e2`). It lists every file's `first_forest`, `last_forest`, `n_forests` and `n_halos`. File 0 holds forests 0 to 236,265 (236,266 forests, 12,434,838 halos). File 32 holds forests 6,897,711 to 7,206,194 (308,484 forests, 7,497,046 halos).
- Its `manifest.json` adds each file's bytes and SHA-256.
- The halo-level check proved this conversion halo by halo, link by link and bit by bit against the horizontal dataset. Mimic's vertical `halos-only` and `sage16` runs on it are bitwise identical to the horizontal runs.

The checks for SAGE26, in order:

1. **Header totals.** For every file, the header's `ntrees` and `totNHalos` equal `n_forests` and `n_halos` in `file_plan.json`. Over the run, SAGE26's `totnforests` and `totnhalos` (`src/io/read_tree_lhalo_binary.c:155-156`) equal 7,206,195 and 406,668,896.
2. **A record spot-check.** For a few halos, the converted `Mvir` × 10¹⁰ equals the stored `M_Crit200` to float32 rounding. `MostBoundID` and `SnapNum` read back exactly. Every link is −1 or inside its forest. Every `FirstHaloInFOFgroup` of a central is its own index.
3. **A complete SAGE26 run** on all 33 files, with `NumSimulationTreeFiles 33`. Measure its peak memory on the 1,250,835-halo forest. Scaled by halo count, this is the first real evidence for the super-forest estimate in section 3.
4. **The id relations** on its output: `GalaxyIndex ÷ 10¹⁵` is the file, `first_forest[file] + SAGETreeIndex` is a `ForestIndex` in range, and `SAGEHaloIndex` is below that forest's halo count.

Only then convert or read the production dataset (361 files, the super-forest alone in file 0).
