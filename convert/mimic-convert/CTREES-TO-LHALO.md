# Consistent-Trees ASCII to L-Halo with `ctrees_to_lhalo.py`

**Purpose**: The manual for `ctrees_to_lhalo.py`, the script that converts Consistent-Trees ASCII merger trees (`tree_*.dat` with `forests.list` and `locations.dat`) into L-Halo binary files the way sage-model's `convert_trees_to_lhalo` did when it wrote the mini-Uchuu L-Halo files of June 2022, the reference conversion, with exactly two deliberate differences. Given the reference conversion's particle mass (`--particle-mass`), the output is byte-identical to the reference conversion's in every field the two differences do not touch, which is a property to check by comparison rather than to assume; `Len` follows the particle mass given, so a run at another particle mass differs from the reference in `Len` alone, on top of the two differences.

The script is standalone. It does not use or change the other converter modules in this directory, any simulation package or any dataset; it opens its inputs read-only and writes only the output files it is asked for. It needs Python 3.12+ and `numpy` (both in `mimic_venv`), and it never starts subprocesses or worker pools: parallelism comes from running several processes over disjoint ranges of output files.

## What it reproduces

The reference is sage-model **as it stood at commit `9218e11` (2022-06-11)**, the last commit before the 2022 files were written. Every line number below is in a file at that commit (`git -C <sage-model> show 9218e11:<path>`); the module docstring of `ctrees_to_lhalo.py` cites the same lines step by step.

Terms: an *ASCII tree* is one `#tree` block of a `tree_*.dat` file; a *forest* is the set of ASCII trees sharing a `ForestID` in `forests.list`, and it is one L-Halo "tree" of the output files.

| Step | sage-model at `9218e11` |
|---|---|
| Read `forests.list` and `locations.dat`, resolve tree files from the directory of `locations.dat`, require contiguous file ids and a perfect-cube file count, pair the two tables by tree id and refuse disagreement | `read_forests`, `read_locations`, `assign_forest_ids`: `src/io/ctrees_utils.c` lines 24-294 |
| Order the trees by (`ForestID`, `FileID`, `Offset`) and number the forests in that order | `read_tree_consistentrees_ascii.c` lines 64-79 |
| Take the column map from the header line of `tree_0_0_0.dat` beside `locations.dat` (sage-model's `SimulationDir/TreeName`) | `parse_header_ctrees`, `src/io/parse_ctrees.h` lines 166-321; `read_tree_consistentrees_ascii.c` lines 21-25 and 242-292 |
| Read a tree's rows from its offset to the next line beginning `#`, parsing `scale`/`desc_scale` as float64, the ids as int64, `snap_num` as int32 and every other column as float32 exactly as `strtof` rounds it | `read_single_tree_ctrees` and `parse_line_ctrees`, `src/io/parse_ctrees.h` lines 323-515 |
| Convert each ASCII tree's conventions (Spin, masses times `1e-10`, `Len = (int) roundf(Mvir * (1/PartMass))`, `FileNr`, `SubhaloIndex`, `SubHalfMass`) | `convert_ctrees_conventions_to_lht`, `read_tree_consistentrees_ascii.c` lines 426-463 |
| Check the forest's last snapshot for FoF centrals (`fix_flybys`), then resolve hosts (`fix_upid`) and build the output order and the FoF, descendant and progenitor links (`assign_mergertree_indices`) | `src/io/ctrees_utils.c` lines 297-735, called from `load_forest_ctrees`, `read_tree_consistentrees_ascii.c` lines 325-424 |
| Split the forests over the output files | `setup_forests_io_ctrees`, `read_tree_consistentrees_ascii.c` lines 83-124 |
| Write each output file (header, then each forest's records) | `convert_trees_to_lhalo`, `src/sage.c` lines 358-528 |

The record is sage-model's `struct halo_data` (`src/core_simulation.h`), 104 bytes, little-endian, which is the layout Mimic's `simulations/mini-uchuu/halo_properties.yaml` declares (`Mvir` there is Mimic's `M_Crit200`).

## The two differences, and why

1. **No flyby demotion.** sage-model's `fix_flybys` (`src/io/ctrees_utils.c` lines 297-381) finds the most massive FoF central at a forest's last snapshot and, when there is more than one, rewrites the others: it negates their `MostBoundID`, sets their `pid` to the survivor (lines 370-371), and sets the `upid` of **every** other halo at that snapshot to the survivor (line 377), subhalos of other FoF groups included. That rewrites the halo catalogue at the last snapshot rather than converting it, and the `upid` rewrite reaches subhalos of other FoF groups as well. The script keeps every other operation of `fix_flybys` (its sort, its search for the last scale and the survivor, and its refusals when the last scale holds no FoF or no survivor is found) and drops only those writes, so several FoF centrals at a forest's last snapshot stay centrals with positive `MostBoundID` and their own FoF groups.
2. **Each halo's own id as `MostBoundID`.** sage-model passes the forest's first row as the `info` of every ASCII tree (`read_tree_consistentrees_ascii.c` lines 357-372, used at line 449), so halo *j* of a later tree in a multi-tree forest got the id of the forest's *j*-th row, *j* the halo's position in its own ASCII tree. That is a bug: the field should carry the halo's own id. The script gives each halo its own row's `id`.

Everything else follows sage-model to the bit, including the floating-point sequence of `Len` and the progenitor chain's insertion order (a later progenitor goes to the front only when strictly more massive, so the chain is not mass-sorted).

## Forests to files

With F forests and N output files, file *t* holds the contiguous run of `floor(F/N)` forests, plus one for the first `F mod N` files, starting at forest `floor(F/N) * t + min(t, F mod N)` (sage-model's split over its `NTasks`). For example F = 10 and N = 3 give forests 0-3, 4-6 and 7-9.

When N is greater than F, sage-model at `9218e11` fails: `setup_forests_io_ctrees` refuses a task with no forests (lines 121-124). The script refuses such a run whole, with exit status 2, whichever files are asked for. That is stricter than sage-model, whose tasks below F would still have written their files.

A count that does not fit a 32-bit integer is refused with exit status 2, as sage-model's `PWRITE_64BIT_TO_32BIT` refuses it (`src/sage.c` lines 421-444): the number of output files, a file's forest count, a forest's halo count and a file's total halo count must each be at most `INT_MAX` (2147483647).

Each file is written as `convert_trees_to_lhalo` writes it, little-endian:

| Bytes | Content |
|---|---|
| `int32 Ntrees` | the file's forest count |
| `int32 totNHalos` | the file's total halo count |
| `int32 TreeNHalos[Ntrees]` | each forest's halo count, in order |
| `Ntrees` blocks of 104-byte records | each forest's records, in order |

## Command line

```bash
mimic_venv/bin/python convert/mimic-convert/ctrees_to_lhalo.py \
    --forests-list PATH --locations PATH --n-files N --files A-B \
    --particle-mass X --tree-name NAME --out DIR
```

Every option is required.

| Option | Meaning |
|---|---|
| `--forests-list PATH` | the Consistent-Trees `forests.list` |
| `--locations PATH` | the Consistent-Trees `locations.dat`; tree files, and the header file `tree_0_0_0.dat`, are resolved from its directory, as sage-model does |
| `--n-files N` | the number of output files the forests are split over (sage-model's `NTasks`) |
| `--files A-B` | write output files A to B inclusive, numbered from 0; A must not exceed B and B must be below N |
| `--particle-mass X` | sage-model's `PartMass`, in 1e10 Msun/h; a positive finite number, with no default |
| `--tree-name NAME` | output files are named `NAME.<t>` |
| `--out DIR` | an existing directory for the output files |

**Several processes.** A run can be split over processes that are given disjoint `--files` ranges with the same other options; each reads the tables itself and writes only its own files. Every output file and every sidecar is written under a temporary name unique to that file (`.<NAME>.<t>.<random>.partial` and `.<NAME>.<t>.<random>.json.partial` in `DIR`) and renamed to its final name only when complete, so concurrent processes never collide and no reader ever sees a partial file under a final name. The processes must be given disjoint ranges: the script checks that a final name is free just before the rename, but two processes racing for the same file are not otherwise prevented.

**No overwriting.** Before reading or writing anything, the script refuses (exit 3) when any output file `NAME.<t>` or sidecar `NAME.<t>.json` for t in A-B already exists. Files outside the range are never touched. To redo a file, remove (or move aside) both its output and its sidecar first.

**On failure** the file being written is abandoned and its temporaries removed; files of the range already completed stay, each with its sidecar.

## Outputs

For each t in A-B, in `DIR`:

- `NAME.<t>`, the L-Halo file described above.
- `NAME.<t>.json`, its sidecar, renamed into place after the data file (so a data file without its sidecar marks an incomplete write). It records: `script_sha256` (SHA-256 of `ctrees_to_lhalo.py`) and `sage_model_commit`; `inputs`, with the absolute path and size in bytes of `forests.list`, `locations.dat`, the header tree file and every tree file this output file read; `output` (its path, size, tree name, file number and N); `forest_range` (`start` and `stop`, forest numbers in sage-model's order with `stop` exclusive, the run's total forest count, and the first and last `ForestID`); `nforests` and `nhalos`; `particle_mass` and the `particle_mass_argument` as typed; and `started_utc` and `finished_utc`.

Progress lines go to standard error.

## Exit statuses

| Status | Meaning |
|---|---|
| 0 | every file of the range was written |
| 2 | a read, parse or consistency error, or a refused count (including N greater than F); the message names the file (and, for a forest, its tree file, forest number and `ForestID`) |
| 3 | a usage error (a missing or malformed option, `--files` with A greater than B or B not below N, a non-positive particle mass, a missing `--out`), or a refused overwrite |

argparse's own usage exit status of 2 is replaced by 3, so 2 always means the data.

Inputs are Consistent-Trees ASCII forests as staged for Uchuu. Where sage-model's behaviour on an input is undefined or would never terminate (an empty forest, repeated tree or halo ids, a missing column, malformed rows, a host chain that never reaches a FoF), the script refuses with exit status 2; the module docstring lists those cases and the one family it resolves where sage-model would loop forever. Behaviour on other inputs is unspecified.

## Example: the mini-Uchuu set

The mini-Uchuu L-Halo set is 128 files named `Uchuu400_Planck_lhalo_binary.<t>`, and the Uchuu suite's particle mass is 0.0327. To convert the mini-Uchuu Consistent-Trees ASCII directory (`<ascii-dir>`, which holds `forests.list`, `locations.dat` and the `tree_*.dat` files) into an empty `<out-dir>` at that particle mass:

```bash
ASCII=<ascii-dir>
OUT=<out-dir>
MASS=0.0327
for k in 0 1 2 3 4 5 6 7; do
    mimic_venv/bin/python convert/mimic-convert/ctrees_to_lhalo.py \
        --forests-list "$ASCII/forests.list" --locations "$ASCII/locations.dat" \
        --n-files 128 --files $((16 * k))-$((16 * k + 15)) \
        --particle-mass "$MASS" --tree-name Uchuu400_Planck_lhalo_binary --out "$OUT" &
done
wait
```

That runs eight processes of sixteen files each and takes about 21 hours for all 128 files, reading the ASCII catalogues from an external disk (the run is limited by random reads of the ASCII trees); any split into disjoint ranges gives the same files. Check that every process exited 0 and that all 128 files and sidecars exist before using the set. Each process holds its own copy of the forest tables in memory.

## The particle mass

`PartMass` enters the conversion only through `Len = (int) roundf(Mvir * (1.0/PartMass))` (`read_tree_consistentrees_ascii.c` line 441 at `9218e11`): the float32 scaled `Mvir` times the double reciprocal gives a double, which is narrowed to float32 and rounded half away from zero. Neither difference touches `Mvir` or `Len`, so every record of a written file constrains the particle mass, and the masses that reproduce a set of `Len` values form an interval of doubles rather than a point.

`particle_mass_interval` in `ctrees_to_lhalo.py` finds that interval exactly from the records' `Mvir` and `Len` (`particle_mass_interval_of_files` reads them from L-Halo files, and `lhalo_len.py check` reports the result): it is a bisection over the bit patterns of the doubles, nothing is sampled, every double between its ends (`low` and `high`) reproduces every `Len`, and none outside does. `shortest_decimal` names the decimal with the fewest significant digits inside the interval (of those, the one nearest its midpoint), which is the `value` that reproduces the files checked, and the result is empty when no particle mass reproduces every `Len`.

The 2022 mini-Uchuu L-Halo files' `Len` are reproduced at 0.0325. The Uchuu suite's particle mass is 0.0327 (1e10 Msun/h).
