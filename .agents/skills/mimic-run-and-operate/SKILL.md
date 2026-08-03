---
name: mimic-run-and-operate
description: Running Mimic and interpreting what it produces. Load when a task involves executing ./mimic with a run YAML, choosing or editing a run file (modules/phases/parameters/output sections), disabling a physics module, running an empty pipeline or halos-only, verbosity and progress output (-v/-d/-q), the --skip or --compress flags, MPI runs (mpirun), chunked output (target_file_size_mb, forests_per_file), reading binary or HDF5 galaxy output (h5py, output_schema.json), the metadata/ reproducibility record, output file naming, snapshot selection (snapshot_list, a_list), or invoking mimic-plot on a finished run.
---

# Mimic Run and Operate

How to run the compiled `mimic` executable, control what it does with a run YAML, and read the artifacts it leaves behind. All paths are relative to the repo root.

## When to use / when NOT to use

Use this skill for: launching runs, editing run files, disabling modules, verbosity/progress, `--skip`/`--compress`, MPI execution, output file layout, reading binary/HDF5 outputs, the `metadata/` record, and the plotting quickstart.

Do NOT use it for:

- Environment setup, prerequisites, venv, make flags and library detection — see the `mimic-build-and-env` skill.
- The full catalog of every run-YAML key, CLI flag, and Make variable with defaults — see the `mimic-config-and-flags` skill.
- Plotting internals (registry, profiles, figure contract, skip diagnostics) — see the `mimic-plots-and-analysis` skill.
- A run that fails, crashes, or rejects its config — see the `mimic-debugging-playbook` skill.
- Adding simulations, readers, or understanding tree formats — see the `mimic-simulations-and-readers` skill.
- Claiming scientific results from a run's output — see the `mimic-scientific-method` skill (numbers before claims).

## First actions

1. Confirm what the executable was built for: `make info` reports the compiled MODEL/SIMULATION pair. The binary embeds one pair; the run file must name the same pair.
2. Read the run file you are about to use, start to finish. It is short. Check `model.name`, `simulation.name`, `output.output_directory`, and the `modules:` section.
3. Confirm input data exists: `ls simulations/<sim>/snapshots/` (a gitignored symlink/directory — see the `mimic-build-and-env` skill if missing).
4. Run, capture output, check the exit code explicitly (commands below). Exit code 0 or it failed — never judge success from log text alone.

## Command anatomy and the golden rule

```bash
./mimic [-h|--help] [-v|--verbose] [-d|--debug] [-q|--quiet] [--skip] [--compress] <run.yaml>
```

**Golden rule:** the executable is compiled for exactly one MODEL/SIMULATION pair (chosen at `make` time; defaults sage16 + mini-millennium). The run file declares `model.name` and `simulation.name`; if either does not match what the binary was compiled for, startup rejects the run before any processing with `Run file selects model.name='X' but this executable was built with MODEL=Y` (and the analogous message for SIMULATION; `src/core/read_parameter_file.c`). There is no runtime model switching — rebuild with the other pair instead.

Canonical build → run → check sequences (each uses one consistent selector pair):

```bash
# Default pair: sage16 physics on mini-millennium
make
./mimic models/sage16/input/sage16_mini-millennium.yaml; echo "rc=$?"

# SHAM model (same simulation)
make MODEL=sham SIMULATION=mini-millennium
./mimic models/sham/input/sham_mini-millennium.yaml; echo "rc=$?"

# Halos-only: empty model package, halo tracking with no galaxy physics
make MODEL=halos-only SIMULATION=mini-millennium
./mimic models/halos-only/input/halos-only_mini-millennium.yaml; echo "rc=$?"
```

## Run-file walkthrough (sage16 on mini-millennium)

`models/sage16/input/sage16_mini-millennium.yaml` is the reference run file. Section roles:

| Section | Role |
|---|---|
| `model.name: sage16` | Must match the compiled MODEL (golden rule above) |
| `simulation.name: mini-millennium` | Must match the compiled SIMULATION; pulls defaults from `simulations/mini-millennium/simulation_info.yaml` (run-file keys override them) |
| `output.output_filename: model` | Base name `<base>` for all output files |
| `output.output_directory: output/sage16-mini-millennium` | Where everything lands (see "Output location" below) |
| `output.output_format: hdf5` | `binary` or `hdf5` (`hdf5` needs an HDF5-enabled build, the default) |
| `output.snapshot_list: [63, 37, ...]` | Which snapshots to write; empty/omitted = all (see "Snapshot selection") |
| `SubSteps: 10` | Integration substeps per snapshot interval (timestep detail: `mimic-config-and-flags`) |
| `modules.pre_timestep:` | Modules run once per snapshot before substepping (sage16: reionization, infall budget, disk radius, merger clock) |
| `modules.phases:` | Ordered user-named substep phases; phase names execute in YAML order, once per substep. Within a phase, dispatch is grouped by processing mode: full-halo modules first, immediate event consumers, then by-galaxy modules; see the `mimic-modules` ordering law. sage16 has `galaxy_physics` then `satellite_mergers` (merger resolution emitting the `merger` event, consumed by `sage_quasar_mode` and `sage_starburst_feedback` in per-event mode) |
| `modules.post_timestep:` | Modules run once per snapshot after substepping (empty for sage16) |
| `modules.parameters:` | Flat name→value map; every parameter a module declares must be present (no defaults — missing parameter fails that module's `init()`) |

Unknown keys inside fixed-schema sections are rejected at startup (`model:`, `simulation:`, `input:`, `output:`, `plotting:`, and `modules:`). Top-level timestep keys (`SubSteps`, `TimestepScheme`, `MaxDynamicSubsteps`) are exact-spelling keys with no top-level whitelist, so a stray top-level key can be ignored; use the `mimic-config-and-flags` skill when editing run-file structure.

### Empty pipeline and halos-only

To run halo tracking with zero galaxy physics you have two equivalent routes:

1. In any run file, empty the pipeline:

```yaml
modules:
  phases: {}
  parameters: {}
```

2. Or build `MODEL=halos-only` and use its shipped run file — the halos-only package IS the empty pipeline as a maintained model package, which is the better choice for anything you will repeat or share.

### Disabling a module safely — transport coupling warning

Deleting one line from `modules.phases:` is only safe for a self-contained module. Many sage16 modules are **calculate/apply pairs** coupled by transport properties (accumulator fields carried between modules within a substep; see the `mimic-properties` skill). Removing one half leaves the other half consuming zeros or accumulating values nobody applies. Verified coupled pairs in the shipped sage16 pipeline:

- `sage_calculate_cooling_budget` → `sage_apply_cooling` (with `sage_radio_mode_heating` adjusting the budget in between)
- `sage_calculate_supernova_feedback` → `sage_apply_star_formation_supernova` (which also consumes `sage_calculate_star_formation`'s output)
- `sage_apply_star_formation_supernova` produces `NewStellarMass`, consumed by `sage_apply_metal_enrichment`

Rule: to disable a physics process, remove the whole coupled chain, and check each module's `module_info.yaml` `dependencies:` block first. Some modules also enforce ordering at init and will FATAL if their partner is missing or misordered — that failure is your friend; do not work around it. Missing parameters for enabled modules fail during module `init()`; extra entries in `modules.parameters:` are currently tolerated, recorded in metadata, and not caught by `scripts/lint_parameter_usage.py` (that linter compares C parameter loads against `module_info.yaml`, not run YAML).

## What a run produces

Everything goes into `output.output_directory`. Both formats ALWAYS get a `metadata/` directory.

**HDF5 format** (`output_format: hdf5`):

- Per-file outputs `<base>_NNN.hdf5` (e.g. `model_000.hdf5`, `%03d` numbering), one per output partition/chunk, each with `Snap<NNN>/Galaxies` (structured galaxy dataset) and `Snap<NNN>/TreeHalosPerSnap` per output snapshot.
- A master file `<base>.hdf5` containing external links to the per-file outputs plus its own `RunProperties`. Open the master for whole-run access; keep it in the same directory as the per-file outputs or the links break.
- `RunProperties/` (in the master AND every per-file output, `src/io/output/metadata_hdf5.c`): `Version` group (git branch/commit, build date), `EnabledModules`, `EventContracts`, `Parameters` (compound datasets — omitted when empty, e.g. an empty pipeline writes no `EnabledModules`), `Redshifts`, and per-file `FieldMetadata` (compound dataset of field_name/units/description rows). **This is the reproducibility record**: months later, the HDF5 file alone recovers exactly which pipeline (modules, order, event wiring) and which parameter values produced it, without the original YAML.

**Binary format** (`output_format: binary`):

- One file per output snapshot per chunk: `<base>_z<z.zzz>_<n>` (e.g. `model_z0.000_0` — redshift to 3 decimals, then chunk number; `src/io/output/util.c`).

**Both formats** also get `example_Mvir_Len_plot.py` auto-written into the output directory (`src/io/output/python_example.c`, called unconditionally from `main.c`) — a runnable, self-contained first-look script.

**metadata/ (both formats)** — verified layout in `tests/data/output/baseline/{binary,hdf5}/metadata/`:

- `output_schema.json` — the binary record layout and field units for THIS run
- a copy of the run YAML, the simulation config, the `a_list`, and `version_info.json` (git version of the code that ran)

**Rule: binary outputs travel WITH their `metadata/` directory.** An old run is read with its OWN `output_schema.json`, never with the current checkout's generated schema — property layouts change between versions, and the run-local schema is the only authoritative description of those bytes.

## Reading outputs

### HDF5 with h5py (venv has h5py)

```python
import h5py

with h5py.File("output/sage16-mini-millennium/model_000.hdf5", "r") as f:
    gals = f["Snap063/Galaxies"]              # structured dataset, one row per galaxy
    mvir = gals["Mvir"][:]
    meta = f["RunProperties/FieldMetadata"][:]  # compound rows: field_name, units, description
    units = {r["field_name"].decode(): r["units"].decode() for r in meta}
```

Structure verified against `tests/data/output/baseline/hdf5/model_000.hdf5`. For quick structure checks use `h5ls -r <file>`; deeper inspection recipes live in the `mimic-diagnostics-and-tooling` skill.

### Binary with the run-local schema

Binary file layout: `int32 ntrees`, `int32 ngalaxies`, `int32[ntrees]` galaxies-per-tree, then `ngalaxies` fixed-size records as described by `output_schema.json`. Use the reader shipped in `plot/mimic-plot/output_schema.py` (functions verified: `load_schema(output_path)`, `dtype_from_schema(schema, *, binary=True)`, `units_from_schema(schema)`, `mass_to_msun(values, unit_label, hubble_h)`):

```python
import sys
import numpy as np

sys.path.insert(0, "plot/mimic-plot")
from output_schema import load_schema, dtype_from_schema, units_from_schema

path = "output/sage16-mini-millennium/model_z0.000_0"
schema = load_schema(path)          # finds <dir>/metadata/output_schema.json automatically
dtype = dtype_from_schema(schema)   # binary=True: offsets + padded record size
units = units_from_schema(schema)   # {field_name: unit_label}

with open(path, "rb") as fh:
    ntrees = np.fromfile(fh, np.int32, 1)[0]
    ngals = np.fromfile(fh, np.int32, 1)[0]
    per_tree = np.fromfile(fh, np.int32, ntrees)
    galaxies = np.fromfile(fh, dtype, ngals)

print(ngals, galaxies["Mvir"][:5], units["Mvir"])
```

## Verbosity, progress, and capture

| Flag | Effect |
|---|---|
| (none) | INFO and above; interactive terminals get an in-place progress bar |
| `-v` / `--verbose` | Adds VERBOSE_LOG lines and context (timestamp, file:line) |
| `-d` / `--debug` | Everything, including rate-limited DEBUG_LOG output |
| `-q` / `--quiet` | Warnings and errors only; progress output suppressed |

Progress behavior: on a TTY the progress bar redraws in place; when stdout is redirected to a file, progress falls back to discrete lines at 5% boundaries so logs stay readable; `--quiet` suppresses progress entirely. Canonical capture (archive/ is a gitignored machine-local dir — `mkdir` it if absent):

```bash
mkdir -p archive/run-logs
./mimic models/sage16/input/sage16_mini-millennium.yaml \
    > archive/run-logs/run.log 2>&1; rc=$?
tail -n 30 archive/run-logs/run.log; echo "rc=$rc"
```

Exit code 0 or it failed — treat any non-zero code as a real failure regardless of log text.

## --skip and --compress

- `--skip`: before processing each partition (input file/chunk), Mimic checks whether ALL of its output files already exist. All present → the partition is skipped (resume/restart support). **Some but not all present → FATAL**: a partial chunk means an interrupted write, and Mimic refuses to guess. Remove the partial files or rerun the whole partition without `--skip`.
- `--compress`: HDF5 only — enables gzip compression on galaxy datasets (off by default). This is byte-level compression: values are bit-identical on read, only file size changes. Silently irrelevant for binary output. There is no YAML key for compression; the CLI flag is the only switch.

## MPI runs

```bash
make USE-MPI=yes          # requires mpicc; note USE-MPI=no still ENABLES it — omit to disable
mpirun -np 4 ./mimic models/sage16/input/sage16_mini-millennium.yaml
```

- Parallelism is over input partitions (tree files for L-Halo formats, enumerated chunks for consistent-trees): ranks divide the partition list; there is no intra-tree parallelism.
- **L-Halo binary guidance:** choose a rank count that divides the tree-file count (mini-millennium ships 8 files: 1, 2, 4, or 8 ranks) so no rank idles.
- **Consistent-trees:** chunk identities are independent of the rank count (NTask), so output chunk numbering — and `--skip` resume — is stable if you rerun with a different `-np`.
- Multi-rank logging: ranks other than 0 reduce console noise; if per-rank detail is missing from the console, check for per-rank log fallback files in the output directory. Verify current behavior with `grep -rn "ThisTask" src/util/error.c src/core/main.c`.

## Chunked output

Two run-YAML knobs under `output:` control how many output files a run produces (details and defaults in the `mimic-config-and-flags` skill):

- `target_file_size_mb`: size-based chunking — Mimic groups forests into chunks aiming at this output size.
- `forests_per_file`: fixed count per output file. Setting it > 0 overrides size-based chunking.
- **Consistent-trees ASCII requires `forests_per_file > 0`** — the ASCII reader cannot cost forests ahead of time, so size-based chunking is unavailable there.
- Resume semantics: chunk boundaries are deterministic for a given config, so `--skip` after an interruption re-derives the same chunk list and skips completed chunks; changing chunking knobs between runs changes the chunk list, making old outputs unmatchable — start a fresh output directory instead.

## Plotting quickstart

```bash
source mimic_venv/bin/activate
python plot/mimic-plot/mimic-plot.py --param-file=models/sage16/input/sage16_mini-millennium.yaml
```

- Figures land under `<output_dir>/plots/`.
- The plot registry is model-local (`models/<model>/plots/figures/`), so the MODEL your build/run used must match the run file you pass — same golden rule as running.
- Fastest first look at a binary run: `python output/<run>/example_Mvir_Len_plot.py` (auto-generated, self-contained).
- Everything deeper (flags, profiles, skip diagnostics, adding figures): the `mimic-plots-and-analysis` skill.

## Output location

- `output.output_directory` may be any writable path. The repo's `output/` is a gitignored machine-local symlink on the maintainer's machine and may not exist on a fresh clone — Mimic creates output directories automatically, so a plain `output/` directory simply appears, or point the key anywhere else.
- Relative paths resolve from the directory you invoke `./mimic` from (the process CWD), not from the run file's location. Invoke from the repo root for the shipped run files to behave as written.

## Snapshot selection

- `output.snapshot_list: [63, 37, ...]` writes only those snapshots; empty list or omitted key = write all snapshots.
- Snapshot indices are defined by the simulation's `a_list` file (`simulations/<sim>/<sim>.a_list`): one scale factor per line, earliest → latest; line N (0-based) is snapshot N. The last line is normally a=1.0, i.e. z=0 — so for mini-millennium's 64 snapshots, snapshot 63 is z=0.
- Selecting fewer snapshots changes only what is WRITTEN; the evolution is always computed through the full tree.

## Provenance and maintenance

Facts verified against the repo on 2026-07-04. Re-verify before trusting anything volatile:

- CLI flags and usage text: `grep -n "usage\|--skip\|--compress\|--quiet" src/core/main.c`
- Mismatch FATAL and unknown-key rejection: `grep -n "FATAL\|model.name\|Unknown" src/core/read_parameter_file.c | head`
- Shipped sage16 pipeline and coupled pairs: `cat models/sage16/input/sage16_mini-millennium.yaml`
- Output naming and metadata contents: `ls tests/data/output/baseline/binary tests/data/output/baseline/hdf5{,/metadata}`
- Binary header/record layout: `grep -n "ntrees\|fwrite" src/io/output/binary.c | head`
- Schema reader API: `grep -n "^def " plot/mimic-plot/output_schema.py`
- Skip/partial-chunk behavior: `grep -rn "skip" src/core/tree_driver.c | head`
- Compression flag plumbing: `grep -rn "compress" src/core/main.c src/io/output/hdf5.c | head`
- Chunking knobs: `grep -n "forests_per_file\|target_file_size_mb" src/core/read_parameter_file.c`
- Progress fallback: `grep -rn "5\|isatty" src/util/progress*.c 2>/dev/null || grep -rln "progress" src/util/`
