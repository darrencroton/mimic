---
name: mimic-build-and-env
description: Set up the Mimic environment from scratch and build correctly. Load when a task involves a fresh clone, first_run.sh, installing prerequisites (libyaml, HDF5, MPI, Python), the mimic_venv virtual environment, make targets and flags (MODEL, SIMULATION, USE-HDF5, USE-MPI, TEST_BUILD, EXTRA_CFLAGS), clean/tidy semantics, generated-code regeneration during builds, missing symlinks (output/, archive/, benchmarks/, snapshots/), platform differences (macOS vs Linux), CI environment parity, or a "why won't it build / why is my environment broken" question.
---

# Mimic Build and Environment

How to recreate the Mimic development environment from nothing, build the executable correctly, and avoid the known environment traps. Mimic is a C program (built with GNU Make) plus a Python toolchain (code generators, tests, plotting) that live side by side; both halves must be set up.

## When to use / when NOT to use

Use this skill for: fresh-clone setup, prerequisite installation, venv problems, build failures caused by libraries or flags, switching MODEL/SIMULATION pairs, understanding what `make` regenerates, and missing-directory/symlink confusion.

Do NOT use it for:

- Running Mimic and interpreting its output — see the `mimic-run-and-operate` skill.
- The full catalog of Make variables, CLI flags, and run-YAML keys — see the `mimic-config-and-flags` skill.
- Test tiers, baselines, and tolerances (including the CI `MIMIC_BASELINE_RTOL` story) — see the `mimic-validation-and-qa` skill.
- Diagnosing failures beyond build/environment (module errors, scientific regressions, memory) — see the `mimic-debugging-playbook` skill.
- Plotting specifics — see the `mimic-plots-and-analysis` skill.

## First actions

1. Run `make info` — it reports compiler, selected MODEL/SIMULATION, full CFLAGS, YAML/HDF5/MPI detection status, the detection method used (pkg-config vs Homebrew vs system paths), and source/object/module counts. This is always the first diagnostic for any build or environment question.
2. Check what exists: `ls -l output archive benchmarks simulations/*/snapshots 2>&1` — on a fresh clone most of these are missing (see "Machine-local symlinks" below); on the maintainer's machine they are symlinks to local disks.
3. Check the venv: `ls mimic_venv/bin/python3 mimic_venv/bin/clang-format 2>&1`.
4. Confirm data: `ls simulations/mini-millennium/snapshots/trees_063.0 2>&1` — if absent, run `./scripts/first_run.sh`.
5. Only then build or edit anything.

## Prerequisites

| Requirement | Status | Install / check |
|---|---|---|
| C compiler (gcc or clang) | Required | `cc --version` |
| GNU Make | Required | `make --version` |
| Python 3.9+ | Required | `python3 --version` |
| libyaml (dev headers) | **Required** — build fails without it | macOS: `brew install libyaml`; Ubuntu/Debian: `sudo apt-get install libyaml-dev`; Fedora/RHEL: `sudo dnf install libyaml-devel` |
| HDF5 dev libraries | Optional (default-on; `USE-HDF5=no` opts out) | macOS: `brew install hdf5`; Ubuntu/Debian: `sudo apt-get install libhdf5-dev` |
| MPI (mpicc) | Optional (only with `USE-MPI=yes`) | macOS: `brew install open-mpi`; Ubuntu/Debian: `sudo apt-get install libopenmpi-dev` |
| wget or curl | Needed once, for the data download | usually preinstalled |

Library detection order (identical logic for libyaml and HDF5, in the `Makefile`): **pkg-config** (`yaml-0.1` / `hdf5`) → **Homebrew** (`brew --prefix libyaml` / `brew --prefix hdf5`) → **system paths** (`/usr/include`, the Ubuntu/Debian serial-HDF5 path `/usr/include/hdf5/serial` with libs in `/usr/lib/x86_64-linux-gnu/hdf5/serial`, then `/usr/local/include`). If libyaml is not found, `make` stops with an error listing the three install commands above. If HDF5 is enabled but not found, the error additionally suggests the escape hatch `make MODEL=<model> USE-HDF5=no`. MPI detection is a presence check only: `USE-MPI` set requires `mpicc` on PATH (or an explicit `CC=<your-mpi-wrapper>`), and the compiler becomes `mpicc` with `-DMPI`.

## Fresh-clone setup: scripts/first_run.sh

```bash
git clone https://github.com/darrencroton/mimic.git
cd mimic
./scripts/first_run.sh
make
```

What the script does, in order (it is `set -e`, so it stops on the first hard failure):

1. Confirms it is running from the repo root (checks for `README.md`, `Makefile`, `src/`).
2. Creates `simulations/mini-millennium/snapshots/` as a real directory.
3. Downloads the mini-Millennium merger trees (~270 MB tar from Dropbox) via wget or curl, extracts into `snapshots/`, deletes the tar. **Skip check:** if `simulations/mini-millennium/snapshots/trees_063.7` already exists, the download is skipped entirely.
4. Verifies Python 3 and pip exist (hard error if not).
5. Checks for libyaml (pkg-config, then `/usr/include/yaml.h`, then `/usr/local/include/yaml.h`) — **warning only**; setup continues, but `make` will later fail without it.
6. Creates the `mimic_venv/` virtual environment if absent, upgrades pip, checks for numpy/matplotlib/tqdm, and runs `pip install -r requirements.txt` if any are missing, then verifies the imports and deactivates.
7. Validates that `models/sage16/input/sage16_mini-millennium.yaml` and `simulations/mini-millennium/simulation_info.yaml` exist.
8. Final validation: warns if `snapshots/trees_063.0` or `simulations/mini-millennium/mini-millennium.a_list` is missing, and reports whether a `mimic` binary exists (it does **not** compile one).

What it does NOT do: it never compiles Mimic (`make` is your job), and it never downloads Uchuu, full-Millennium, or any other production simulation data — those `simulations/*/snapshots` directories must be symlinked to data you obtain yourself (see the `mimic-simulations-and-readers` skill for what each package expects).

Manual fallback (documented in `docs/USER-GUIDE.md` "Manual Setup") if the script fails:

```bash
mkdir -p simulations/mini-millennium/snapshots
cd simulations/mini-millennium/snapshots
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" \
     -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar && rm mini-millennium-treefiles.tar
cd ../../..
python3 -m venv mimic_venv
source mimic_venv/bin/activate && pip install -r requirements.txt && deactivate
make
```

## The mimic_venv virtual environment

`requirements.txt` installs: numpy, matplotlib, tqdm (plotting); h5py, PyYAML (integration/scientific tests and HDF5 inspection); black, isort, and a **pinned `clang-format>=20,<21`** (formatting — the pin keeps formatting byte-identical across machines and CI).

- Activate: `source mimic_venv/bin/activate` — deactivate: `deactivate`.
- What needs it: `plot/mimic-plot/mimic-plot.py`, the Python test tiers, `./scripts/beautify.sh`, and `make check-format`.
- The Makefile resolves tools itself: `PYTHON` uses `mimic_venv/bin/python3` only when the venv exists **and is currently activated** (it checks `$VIRTUAL_ENV`); `CLANG_FORMAT` uses `mimic_venv/bin/clang-format` whenever the file exists, activation not required. `beautify.sh` likewise prefers the venv clang-format. Practical consequence: run test targets with the venv activated so `PYTHON` picks up h5py/PyYAML.
- Rebuild recipe (venv broken or wrong Python): `rm -rf mimic_venv && python3 -m venv mimic_venv && source mimic_venv/bin/activate && pip install -r requirements.txt`.

## Build recipes and semantics

```bash
make                                   # defaults: MODEL=sage16 SIMULATION=mini-millennium
make MODEL=sham SIMULATION=mini-millennium   # another pair; keep the SAME pair for
                                             # generate / validate-modules / tests / run
make SIM=mini-millennium               # SIM is shorthand for SIMULATION
make USE-HDF5=no                       # binary output only (see consequences below)
make USE-MPI=yes                       # requires mpicc (or CC=<mpi-wrapper>)
make -j$(sysctl -n hw.ncpu)            # parallel build, macOS
make -j$(nproc)                        # parallel build, Linux
make info                              # first diagnostic: config + library detection
```

Selector rules and traps:

- Lowercase `model=`, `simulation=`, or `sim=` is a hard Make error (case-sensitivity guard), not a silent ignore.
- An unknown `MODEL`/`SIMULATION` fails loudly (`Unknown MODEL ...`) for every target except the model-free set: `clean tidy help check-docs check-format test-clean summary`.
- **`USE-MPI` is checked with `ifdef`: `make USE-MPI=no` still ENABLES MPI.** To build without MPI, omit the variable entirely.
- `USE-HDF5` defaults to yes; any value other than `yes` disables it.
- `EXTRA_CFLAGS` (e.g. `EXTRA_CFLAGS="-O3 -march=native"`) is for benchmarking/profiling only, never production builds.

`USE-HDF5=no` consequences: all `*hdf5.c` sources are excluded, so (a) only binary output works — a run file with `output.output_format: hdf5` fails at startup with `FATAL: Recompile with HDF5 enabled (default) or remove USE-HDF5=no`, and (b) the HDF5 tree readers (`lhalo_hdf5`, `consistent_trees_hdf5`) are not registered, so selecting one via `input.tree_type` fails with `Unknown tree_type`. Note the shipped default run file `models/sage16/input/sage16_mini-millennium.yaml` uses `output_format: hdf5`, so a `USE-HDF5=no` build cannot run it unchanged.

Clean semantics:

| Target | Removes | Keeps |
|---|---|---|
| `make clean` | `build/` (both object trees), the `mimic` executable, plus everything `test-clean` removes | generated source in `src/*/generated/` (regenerated on next make anyway) |
| `make tidy` | the current mode's build dir only (`build/`, or `build/test` under `TEST_BUILD=yes`) | the executable |
| `make test-clean` | `tests/unit/build/`, `tests/data/output/{binary,hdf5}/*`, test `__pycache__`/`.pyc` | everything else |

`TEST_BUILD=yes` builds a test-instrumented executable: it uses a **separate object tree `build/test/`**, exports `MIMIC_TEST_BUILD=1` to the generators, compiles the framework fixture/event modules under `src/module_system/test_*`, and merges the test-only property metadata (`TestDummyProperty`). The executable is still named `mimic` so test harnesses find it. Two mode markers make flag/selector switches safe: `build/.last_exec_mode` forces a **relink** when TEST_BUILD/USE-HDF5/USE-MPI/MODEL/SIMULATION change, and `$(BUILD_DIR)/.last_compile_mode` (also tracking CC and EXTRA_CFLAGS) forces a **recompile**. So switching pairs or flags is just `make MODEL=... SIMULATION=...` — no manual clean needed, though `make clean` is the reset if anything looks inconsistent. The test targets set `TEST_BUILD=yes` themselves; you rarely type it by hand.

## Generated code during builds

Property and module registration code is generated from YAML metadata; **never hand-edit anything under a `generated/` directory** — route changes through the YAML and the generators (see the `mimic-properties` and `mimic-modules` skills).

- Plain `make` regenerates automatically: stamp files (`build/generated/property_generation.stamp`, `build/generated/module_registry.stamp`) have a `FORCE` prerequisite, so the generators run on **every** make invocation; each generator then applies its own hash gate (e.g. `build/generated/property_hash.txt`) and prints "unchanged - skipping regeneration" when nothing changed. This is why switching MODEL/SIMULATION cannot reuse a stale schema.
- Run `make MODEL=<m> SIMULATION=<s> generate` explicitly when you want regenerated files without compiling — e.g. to inspect generated headers after a YAML edit, or to refresh `tests/generated/` for tooling. It runs `generate_properties.py`, `generate_module_registry.py`, and `generate_test_inputs.py`.
- `make check-generated` is the drift check (CI runs it): it recomputes the input hash and compares against the `Source MD5:` header embedded in each generated file. Failure means committed generated code does not match its YAML — fix by running `make generate` with the right selectors, never by editing the generated files.
- `make lint-parameters` runs automatically before every build (`validate-build`), so a parameter declaration/usage mismatch fails the build itself.

## Machine-local symlinks: the fresh-clone trap

On the maintainer's machine, several paths are symlinks to local disks. All are gitignored, so **a fresh clone has none of them**:

| Path | Purpose | If missing |
|---|---|---|
| `output/` | Run results (run files default to `output/<run-name>/`) | Harmless — Mimic creates output directories automatically; a real `output/` dir simply appears. Prior results obviously won't be there. |
| `archive/` | Project convention for logs and retired files (`archive/test-logs/`, `archive/fuzz-logs/`) | `mkdir -p archive` before capturing test logs; it stays gitignored. |
| `benchmarks/` | `scripts/benchmark_mimic.sh` JSON results | `mkdir -p benchmarks` (the script needs it; gitignored). |
| `obsidian-inbox/` | Maintainer's note-export destination | Ignore unless a task explicitly targets it. |
| `sage-code/` | Local checkout of original SAGE for parity work | Only needed for SAGE-parity comparisons; formatting/build exclude it either way. |
| `simulations/*/snapshots/` | Merger-tree input data per simulation package | The ONLY one first_run.sh populates is `mini-millennium` (real dir + download). Every other package needs your own symlink or copy: `ln -s /path/to/your/data simulations/<sim>/snapshots`. A run against a missing `snapshots/` fails at input reading; tests that need absent production data must SKIP cleanly, not fail (see the `mimic-validation-and-qa` skill). |

Never hard-code the maintainer's paths; create real directories or your own symlinks.

## Platform notes

- **macOS:** libraries come from Homebrew; the Makefile finds them via `brew --prefix`, no env vars needed. Parallel build count: `sysctl -n hw.ncpu`.
- **Ubuntu/Debian:** `libhdf5-dev` installs the *serial* layout, which pkg-config sometimes misses; the Makefile explicitly probes `/usr/include/hdf5/serial` + `/usr/lib/x86_64-linux-gnu/hdf5/serial`, so plain `apt-get install libhdf5-dev libyaml-dev pkg-config` is sufficient. Parallel build count: `nproc`.
- **Portability rule (AGENTS.md):** C must compile clean under both macOS Clang and Linux GCC/mpicc with `-Wall -Wextra -Wshadow -Wformat-security -Wundef`. GCC-only warnings like `-Wformat-truncation` (bounded `snprintf` into maybe-too-small buffers) do not show up on a Mac — either prove the destination is large enough or handle truncation explicitly, so supercomputer builds stay clean.
- Tool resolution: the Makefile's `PYTHON` and `CLANG_FORMAT` variables prefer `mimic_venv` as described above, falling back to whatever `python3`/`clang-format` are on PATH — a formatting mismatch between machines usually means one of them fell back to a system clang-format outside the `>=20,<21` pin.

## CI parity (.github/workflows/ci.yml)

CI runs one job on `ubuntu-latest`: checkout → setup Python 3.x → `apt-get install libhdf5-dev libyaml-dev pkg-config` → `pip install -r requirements.txt` → then, in order: `make check-format` → `make clean && make` → `make check-generated` → `make check-docs` → `make validate-modules` → `make tests-unit` → `make tests-integration` → `make tests-scientific` (the last with `MIMIC_BASELINE_RTOL=1e-3`, because the physics baseline was generated on macOS and Linux libm reproduces it only to ~7e-4 — tolerance rationale in the `mimic-validation-and-qa` skill). On failure it uploads `tests/unit/build/*.log` and `tests/data/output/binary/metadata/`. To reproduce CI locally, run those exact targets in that order with default selectors; the commonest local-vs-CI differences are (1) an unpinned system clang-format, (2) forgetting `check-generated`, (3) macOS-vs-Linux float differences in scientific tests.

## Environment smoke test

Run this sequence after any environment change (new machine, new libraries, venv rebuild). Same selector pair throughout; long outputs go to logs with explicit exit-code checks:

```bash
make info                                              # 1. verify detection before building
mkdir -p archive/test-logs
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)
make -j"$JOBS" > archive/test-logs/build.log 2>&1
rc=$?; tail -n 40 archive/test-logs/build.log; echo "build_rc=$rc"; test "$rc" -eq 0 || exit "$rc"
./mimic models/sage16/input/sage16_mini-millennium.yaml \
    > archive/test-logs/run.log 2>&1
rc=$?; tail -n 40 archive/test-logs/run.log; echo "run_rc=$rc"; test "$rc" -eq 0 || exit "$rc"
source mimic_venv/bin/activate
make tests-scientific > archive/test-logs/sci.log 2>&1; rc=$?
tail -n 30 archive/test-logs/sci.log; echo "sci_rc=$rc" # 3. fast (~30s) scientific tier
```

Treat any non-zero exit code as a real failure regardless of what the log text looks like. If all three pass, the environment is sound; escalate remaining problems via the `mimic-debugging-playbook` skill.

## Provenance and maintenance

All facts verified against the repo on 2026-07-03. Re-verify before trusting anything volatile:

- Defaults and selector guards: `grep -n 'DEFAULT_MODEL\|DEFAULT_SIMULATION\|MODEL_FREE_TARGETS' Makefile`
- Library detection order and error text: `grep -n 'YAML_FOUND\|HDF5_FOUND\|mpicc' Makefile`
- MPI ifdef trap: `grep -n 'ifdef USE-MPI' Makefile`
- Download URL and skip check: `grep -n 'dropbox\|trees_063' scripts/first_run.sh`
- Venv package list and clang-format pin: `cat requirements.txt`
- PYTHON/CLANG_FORMAT resolution: `grep -n 'PYTHON :=\|CLANG_FORMAT :=' Makefile`
- Mode markers and test object tree: `grep -n 'last_exec_mode\|last_compile_mode\|build/test' Makefile`
- Generator hash gate: `grep -n 'property_hash' scripts/generate_properties.py`
- HDF5-disabled fatal paths: `grep -n 'USE-HDF5=no' src/core/read_parameter_file.c`
- CI step order and env: `cat .github/workflows/ci.yml`
- Gitignored machine-local paths: `grep -n 'snapshots\|^archive\|^benchmarks\|^/output\|sage-code' .gitignore`
