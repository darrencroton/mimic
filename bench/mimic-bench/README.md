# Mimic Bench: Evaluating a Model Over a Parameter Design

**Purpose**: The manual for `bench/mimic-bench/`, the offline tool that turns a declared spec (a model, a simulation, a run-file template, parameter ranges, one statistic and one constraint) into a design of Mimic evaluations, runs and verifies them with provenance, and applies the first method built on that boundary: history matching with emulators, which ends in a verdict of pass, fail or error with its evidence. It sits beside `plot/mimic-plot/` and `convert/mimic-convert/` as Mimic's third standalone tool; most Mimic users never need it.

The bench consumes Mimic through its public interface only: a run file in, an HDF5 output and its metadata out, read with `plot/mimic-plot/hdf5_reader.py` and `plot/mimic-plot/output_schema.py`. It names no model, parameter or galaxy property in code; everything specific to a model lives in a spec under `specs/` and a constraint table under `constraints/`. For the short version in context see the [User Guide](../../docs/USER-GUIDE.md#evaluating-a-model-over-a-parameter-design); for how the bench relates to the rest of the tree, see the [Developer Guide](../../docs/DEVELOPER-GUIDE.md#the-bench).

## Contents

1. [What it is for, and what it is not](#what-it-is-for-and-what-it-is-not)
2. [Setup](#setup)
3. [The stages](#the-stages)
4. [What each stage writes](#what-each-stage-writes)
5. [The spec](#the-spec)
6. [The noise model](#the-noise-model)
7. [What binds a capture: the request hash and the manifests](#what-binds-a-capture-the-request-hash-and-the-manifests)
8. [Worked example](#worked-example)
9. [Using an external design](#using-an-external-design)
10. [A spec for another model package](#a-spec-for-another-model-package)
11. [Controls and tests](#controls-and-tests)
12. [Limits](#limits)
13. [Module map](#module-map)
14. [Documentation Directory](#documentation-directory)

## What it is for, and what it is not

**It is a shared evaluation boundary** for methods that need many Mimic runs. The boundary does four things and nothing else. It builds a design (a table of parameter vectors) from a spec. It runs each row with the repository's own `./mimic`, from a run file made from the spec's template, and refuses to trust a run until the run's own record confirms what was asked. It reduces each run to counts of galaxies in the bins of one statistic. And it writes everything with provenance: the inputs it read, the code and libraries that ran, and a two-level SHA-256 manifest over every file. A method then reads a capture and nothing else.

**Its first method is history matching with emulators.** Fitted to a few hundred runs, does an emulator's predictive variance describe its own error well enough that the history-matching rejection rule can be trusted, and how much of the prior box does that rule then remove? Two emulators are fitted per output, named in the spec's `emulators` list in verdict order:

- `gaussian_process` (the primary): scikit-learn's `GaussianProcessRegressor` with a constant times an anisotropic squared-exponential kernel plus white noise, on a standardised output, with the spec's declared count noise as a known variance at each training point;
- `bayes_linear` (the comparator): the PRISM form of a Bayes linear emulator (van der Velden et al. 2019), a polynomial regression up to total order 3 whose terms are chosen by forward stepwise selection under effect heredity, plus a correlated residual with one isotropic Gaussian correlation length over the active inputs and a nugget, in universal-kriging form; the correlation length and nugget come from fixed grids by leave-one-out predictive density.

The verdict is `pass` when at least one candidate passes every criterion (naming the first that does, and saying whether it is the primary), `fail` when none does, and `error` when the capture or the analysis cannot be trusted.

**It is not a calibrator.** It does not search for best-fit parameters, run an optimiser, or tell you which model is right. A `pass` says that, for this spec's statistic and constraint, the named emulator was calibrated on a holdout of Mimic runs and that its rejection rule removes a stated share of the prior box without discarding runs the simulator keeps. A `fail` says no candidate can yet be trusted to rule space out. Neither says the model reproduces the data.

**What it supports**: one statistic (a mass function of one mass-valued galaxy property at one snapshot), one shape of constraint (a value with a symmetric error per bin), one noise model (`poisson_log10`), continuous parameters only, one wave of design (a pilot, a training set and a holdout set, with no refocusing), and whole-box runs only (every tree file of the simulation). See [Limits](#limits).

## Setup

Python 3.12+ and the packages pinned in `requirements.txt` at the repository root: `numpy`, `scipy`, `h5py`, `PyYAML`, `matplotlib`, `scikit-learn` and `threadpoolctl`. If you used `scripts/first_run.sh`, `mimic_venv` already holds them; otherwise, from the repository root:

```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
```

`design` and `analyse` need nothing else, and `analyse` needs no Mimic build at all: it reads a capture's small files, the spec and the constraint table. `capture` runs Mimic, so it needs an executable `./mimic` built for the spec's model and simulation, with the simulation's tree files in place. The shipped spec names `sage16` and `mini-millennium`, the build defaults:

```bash
make MODEL=sage16 SIMULATION=mini-millennium
```

The bench's Mimic root is the repository the tool lives in, found from the package's own path; it is never the working directory and there is no option to point it elsewhere. Run every command below from the repository root, with the repository's interpreter. The bench reads Mimic's executable, the spec's run-file template, `simulations/<simulation>/simulation_info.yaml` and the two helper modules under `plot/mimic-plot/`, and writes nothing in the checkout.

## The stages

Four stages, each a subcommand of `mimic-bench.py`:

| Stage | Command | What it does |
|---|---|---|
| `design` | `mimic_venv/bin/python bench/mimic-bench/mimic-bench.py design --spec <spec> --out <file.csv>` | Writes the spec's design table: one row per evaluation, with its identifier, its set and every parameter value. Deterministic: the same spec gives byte-identical files. |
| `capture` | `mimic_venv/bin/python bench/mimic-bench/mimic-bench.py capture --spec <spec> --sets <names> --out <dir> [--workers 8] [--timeout-s 120] [--design <csv>]` | The only stage that runs Mimic. Writes a run file per row, runs `./mimic -q` on it, verifies the run from what it recorded, and counts galaxies per bin. Refuses an `--out` that exists and ends with two SHA-256 manifests. |
| `select-outputs` | `mimic_venv/bin/python bench/mimic-bench/mimic-bench.py select-outputs --spec <spec> --capture <dir>` | Prints the core and exploratory outputs the spec's selection rule gives for a pilot capture, with the counts behind each bin. Writes nothing. |
| `analyse` | `mimic_venv/bin/python bench/mimic-bench/mimic-bench.py analyse --spec <spec> --capture <dir> --out <dir>` | Fits the spec's emulators on the capture's `train` rows, judges them on its `holdout` rows, applies the rejection rule, and writes `verdict.json`, tables and figures under `--out`, which it refuses if it exists. |

Options:

- `--spec <file>`: the campaign spec (YAML). Every stage takes it.
- `--sets <names>`: comma-separated design sets, `pilot` or `train,holdout`. Required by `capture` for the spec's own design; with `--design`, the set labels to keep (every row when omitted). Rows run in design order, not in the order named.
- `--design <csv>`: capture this design table instead of the spec's (see [Using an external design](#using-an-external-design)).
- `--workers <n>`: runs at once during `capture` (default 8). Workers are threads, each waiting on one `./mimic` subprocess, so `capture` also runs inside an OS sandbox that denies the semaphores a process pool needs.
- `--timeout-s <s>`: seconds one run may take (default 120). A run that exceeds it is killed and counts as failed.
- `--capture <dir>`: a capture directory written by `capture`. `--out <path>`: for `design`, the CSV to write (an existing file is replaced); for `capture` and `analyse`, a directory that must not exist (`capture` creates missing parents, `analyse` does not create the parent of `--out`).
- `--version`, `--help`.

**Exit statuses:**

- `0`: success; for `analyse`, a verdict of **pass**.
- `1`: for `analyse`, a verdict of **fail**. No other stage returns 1.
- `2`: an **error**: an invalid spec, design or constraint table, an unreadable, altered or unwritable file, a usage mistake, a capture with any run that did not verify (it still writes every file and records the failure in `runs.csv`), that was interrupted, whose runs reported differing Mimic versions or commits, `BoxSize` or `Hubble_h`, or whose inputs changed during it (`select-outputs` and `analyse` refuse such a capture), or, for `analyse`, a capture that fails its checks or a verdict that cannot be measured. An unexpected exception also exits 2, with `error: internal error:` and the traceback on stderr, so 1 always means a measured fail.

A capture that cannot start (an existing `--out`, a bad spec or design, a template that cannot carry the design, no executable, a tree file of the run's range absent, for `lhalo_binary` trees, or a `tree_type` whose tree files the bench does not check) exits 2 having written nothing. Neither `capture` nor `analyse` overwrites or resumes: a capture is whole or it is an error, and a second capture of the same spec is a second measurement. Ctrl-C or SIGTERM stops a capture cleanly: no queued run starts, running `./mimic` process groups are terminated, every file is still written with the runs marked `interrupted` and `"interrupted": true` in `context.json`, and the exit status is 2.

## What each stage writes

**`design`** writes one CSV: columns `id`, `set`, then one column per real parameter in spec order, then the inert control. Identifiers are `<set>-<index>` with the index counting from zero within the set. Within a set the rows are, as the spec asks: one row with every dimension at its default, the corners of the real parameters' box (the inert control at its default; binary counting order with the first parameter varying slowest, `low` before `high`), then Latin-hypercube points, stratified in each dimension's own scale. Each set draws from its own random stream derived from the spec's seed and the set's name, so resizing one set never changes another.

**`capture`** writes, under `--out`:

- `context.json`: the command line and arguments; the package's name, version, git commit and whether its files have local changes; the versions of numpy, scipy, h5py, PyYAML, scikit-learn and matplotlib and the BLAS and LAPACK libraries found; the Python version; the host; start and end times (UTC); the **request hash** (`request_sha256`); the spec's path and SHA-256; the constraint table's path and SHA-256; the design's source (`spec` or the CSV's path) and SHA-256; the Mimic root, the version and commit every run reported (one value, or the distinct values if they differ), the `box_size` (`BoxSize`, Mpc/h) and `hubble_h` (`Hubble_h`) they reported, and the SHA-256 of `./mimic`, of the run-file template, of `simulation_info.yaml` and of the two helper modules; the tree files' identity (`trees`: their directory with every link resolved and, for `lhalo_binary` trees, each file's name and size in bytes); and the outcome (`runs`, `verified`, `fatal`, `interrupted`, `exit_status`).
- `design.csv`: the rows that were run.
- `runs.csv`: per run, `id`, `set`, `exit_status`, `wall_time_s`, `verified`, `rows` (galaxy rows at the spec's snapshot) and `failure` (every failed check, joined).
- `outputs.csv`: per run, the galaxy count in every candidate bin, one column per constraint point, named by its unconverted `log10_mass` as written in the table.
- `bins.csv`: per `outputs.csv` column, the statistic's property and snapshot, the bin width and the bin's converted centre and edges in log10 solar masses (a galaxy counts in a bin when `low <= log10(mass / Msun) < high`). The centre and edges are empty when the runs reported no single `Hubble_h`.
- `runs/<id>/`: for each evaluation, the run file, `mimic.log` (stdout and stderr) and Mimic's output under `output/`.
- `manifest.sha256` and `runs/manifest.sha256`: see [What binds a capture](#what-binds-a-capture-the-request-hash-and-the-manifests).

A run counts as **verified** only when it exited 0, Mimic wrote its completion marker (`metadata/version_info.json`), the marker's run-file MD5 equals that of the run file written (so Mimic read those bytes), and the master file's `RunProperties` records exactly the template's parameter names with the values the design asked for, the spec's model and simulation, the run file's tree-file range and `SubSteps`, and the spec's snapshot. Mimic accepts a misspelled run-file key, so a run is trusted only after it has run.

Before the first run, `capture` checks the run's tree files by `input.tree_type` (the simulation's `simulation_info.yaml`, with the run file's `input` keys over it), compared case-insensitively as Mimic does. For `lhalo_binary`, a per-file reader that only warns about a missing file while the statistic divides by the whole box, it requires every tree file of the run's range, `<simulation_dir>/<tree_name>.<n>` for `n` from `first_file` to `last_file`, to exist. For `consistent_trees_ascii`, `consistent_trees_hdf5` and `horizontal_hdf5`, which open one dataset and stop with an error if it is missing, it records only the resolved tree directory. Any other `tree_type`, `lhalo_hdf5` included (also a per-file reader that skips a missing file with a warning), is refused before any run, because the bench does not check that reader's tree files. After the last run it hashes the template, `simulation_info.yaml` and the helper modules again, and `./mimic` again by the path that is launched (so a symlink retargeted during the capture counts), and looks at the tree directory again, and at each tree file's size and modification time for `lhalo_binary`; any difference makes the capture exit 2, with `fatal` naming each file that changed during the capture.

**`select-outputs`** prints two lists, `core: [...]` and `exploratory: [...]`, in the form the spec's `outputs` section takes, then one line per candidate bin with its converted centre, whether the constraint's error is not below its value, the pilot's default-row count, the smallest Latin-hypercube count, how many Latin-hypercube runs meet the rule's galaxy minimum, and its status and reason. It needs a finished capture with exit status 0 whose every run verified, whose `pilot` rows in `design.csv` are, row for row, those `design` writes for this spec's pilot set (identifiers and set labels equal, each value equal to a relative tolerance of 1e-12, so a last-bit difference between machines is accepted), and in which each of those rows has a record in `runs.csv`.

**`analyse`** writes, under `--out`:

- `verdict.json`: the verdict word and the exit status (the same), a message, the passing and primary configurations and whether the primary passed, any errors, the candidates, the noise model's kind and scale, the number of floored values (zero counts set to one), the count scale (`BoxSize`, `Hubble_h`, bin width), the settings used (core outputs, budgets, gate budget, rejection rule, criteria, the box sample's size and seed), every configuration's criteria with the measured value and the threshold of each, and the provenance: the package identity, the library versions, the capture's path and the SHA-256 of its manifest, the SHA-256 of the spec, the request hash and the capture's own package identity. It is written last of the files the verdict depends on, so its word is always the exit status's.
- `tables/diagnostics.csv` (per configuration and core output: the calibration measures) and `tables/budgets.csv` (per configuration and discrepancy budget: the rejection rule's results): the evidence the verdict is computed from. `tables/fits.csv` records what each fit reports about itself (terms, active inputs, correlation length and nugget, or length scales and white noise; residual variance and whether it is at its floor; whether a grid value is at an end of its grid; the Gaussian process's optimiser warnings). It enters no criterion.
- Supplementary tables under `tables/`, written after `verdict.json` and entering no criterion and no verdict: `no_noise_diagnostics.csv`, `no_noise_budgets.csv`, `prediction_only_diagnostics.csv` and `prediction_only_budgets.csv` (each family fitted without the noise model, judged with none and with noise at prediction only; named `<family>_no_noise` and `<family>_prediction_only`), `learning_curve.csv`, `count_bands.csv` (calibration by predicted count), `calibration_by_gate.csv`, `holdout_points.csv` (the per-run holdout values), `transformations.csv` (other emulated quantities), `training_loo.csv` (a training leave-one-out diagnostic), `exploratory.csv` (the exploratory outputs' diagnostics) and `profiles.csv` (per-parameter profiles of the non-implausible fraction for the passing configuration, the primary when none passes). `tables/supplementary_status.csv` records, per table and for the figures, whether it was `written` or `failed` and why; a failed supplementary table is a printed warning and changes neither `verdict.json` nor the exit status.
- `figures/*.png`, drawn only from the tables: `standardised_errors.png`, `predicted_vs_run.png`, `non_implausible_fraction.png`, `learning_curve.png` and `profiles.png`.

Every table opens with `#` lines naming its title, its role (a verdict table is the evidence behind `verdict.json`; a supplementary table enters no criterion) and one line per column with its description and unit. Tables are written atomically, whole or not at all.

**How the verdict is decided.** A configuration passes when all four hold: it is **calibrated** (at least `criteria.calibrated_min_fraction` of the core outputs are validated, each on its own three per-output criteria); at the gate budget the simulator keeps at least `criteria.false_exclusion_min_kept_runs` holdout runs, so the false-exclusion rate can be measured; that **false-exclusion rate** (among the holdout runs the simulator keeps, the fraction the emulator rejects) is at most `criteria.false_exclusion_max_rate`; and the **non-implausible fraction** of the box (the share of 100,000 Latin-hypercube points over the whole design box that the emulator keeps) is at most `criteria.non_implausible_max_fraction`. The rejection rule is applied over the configuration's validated core outputs only, and a configuration with fewer validated outputs than the rule's `rank` has no implausibility and cannot pass. The verdict is `error`, taking precedence, when the capture fails a check, when a calibrated configuration leaves too few kept holdout runs to measure its false-exclusion rate, or when the analysis itself fails; otherwise `pass` when a configuration passes (naming the first in candidate order) and `fail` when none does.

## The spec

A spec is one YAML file read top to bottom as the statement of a campaign, with units in its comments. `specs/sage16-smf.yaml` is the shipped one. The loader is strict: an unknown key at any level, a missing key, an invalid value and a repeated key are all errors, and each message starts with the dotted path of the key (for example `parameters[2].low`), so a misspelled key is an error and never a silent default. Scalars follow the YAML 1.2 core schema: `063` is the integer 63, `1e-2` is a float, and only `true` and `false` are booleans. Two paths resolve against different bases: `run_file.template` against the Mimic root, and `constraint.table` against the spec file's own directory, so a spec runs from any working directory.

| Key | What it declares |
|---|---|
| `model`, `simulation` | The model package and simulation package names, as in `model.name` and `simulation.name` of the run file. Verified against what every run records. |
| `run_file.template` | A run file of that package, relative to the Mimic root. Every scalar the spec does not set is written back exactly as the template has it. |
| `run_file.overrides` | Dotted run-file paths set before the design's parameters (at least the snapshot list the statistic reads). Each must name a key the template already has. An override may not set what the bench owns: a design parameter, `output.output_directory`, `output.output_filename`, `output.output_format` or the tree-file range `input.first_file` and `input.last_file`. |
| `parameters` | The real design parameters, each with `name` (a key of the template's `modules.parameters`), `low`, `high`, the template's own value as `default` (which must lie in the range; a default that differs from the template's stops `capture` before any run) and `scale`: `log` (uniform in log10 of the value; bounds positive) or `linear` (uniform in the value, so a parameter may be zero or negative). |
| `inert_control` | A parameter sampled like the others but carried by no run file, so the analysis must find it unconstrained. It is in every design and among the emulators' inputs. |
| `design` | `seed` and the three sets `pilot`, `train` and `holdout`, each with `latin_hypercube` (a count of points) and optionally `include_defaults` and `include_corners`. |
| `statistic` | The measured quantity: `kind: mass_function`, the mass-valued galaxy `property` (converted to solar masses with the run's own output schema and `Hubble_h`), the `snapshot`, `selection: positive` (galaxies of every type whose property exceeds zero), `bin_width_dex` in dex and `bin_centres: constraint` (each bin is centred on the constraint point's converted mass). Only these values of `kind`, `selection` and `bin_centres` exist. |
| `noise_model` | `kind: poisson_log10` and `scale`, a positive dimensionless multiplier. See [The noise model](#the-noise-model). |
| `outputs` | `selection_rule` (`min_bin_centre` in log10 Msun; `require_error_below_value`; `min_galaxies` per bin; `min_runs` of the pilot's Latin-hypercube runs, which may not exceed the pilot's count) and the two lists `core` and `exploratory`, in the constraint table's `log10_mass` values. A bin is core when its converted centre is at least `min_bin_centre`, the constraint's error is below its value (when required) and at least `min_runs` pilot Latin-hypercube runs hold at least `min_galaxies` galaxies in it. A bin that meets the first two conditions, fails the third and holds at least `min_galaxies` galaxies in the pilot's default row is exploratory. The lists are empty until `select-outputs` fills them from a pilot. |
| `constraint` | `table`, a CSV with the header `log10_mass,phi,sigma_phi` (`#` comment lines and blank lines are ignored; each `log10_mass` once), and `conversion`, the declared bringing of the table to the run's conventions: converted log10 mass is `log10_mass + mass_h_power * log10(h) + mass_shift_dex`, converted density is `phi * h ** density_h_power`, and `hubble_h: run` takes h from the run's recorded `Hubble_h` (the only supported value). |
| `discrepancy` | `budgets_dex`, the model discrepancies scanned in dex, and `gate_budget_dex`, the one of them the criteria are judged at. |
| `implausibility` | `rank` and `threshold`: a point is rejected when the `rank`-th largest implausibility over the validated core outputs exceeds `threshold` standard deviations. |
| `criteria` | `per_output` (`within_sigma` with `within_min_fraction`, `beyond_sigma` with `beyond_max_fraction`, and `min_rms`: an output is validated when at least that fraction of its holdout standardised errors lies within the first bound, at most that fraction lies beyond the second, and their root-mean-square is at least `min_rms`) and the campaign-level `calibrated_min_fraction`, `false_exclusion_max_rate`, `false_exclusion_min_kept_runs` and `non_implausible_max_fraction`. |
| `emulators` | The candidates, in verdict order: the first is the primary. Known: `gaussian_process`, `bayes_linear`; each at most once. |

The implausibility of the emulator at output k is `|E_k - z_k| / sqrt(Var_k + v(N_pred) + sigma_k^2 + b^2)`, with E and Var the emulator's mean and own variance, `v(N_pred)` the declared count noise at the predicted count, `z_k` and `sigma_k` the converted constraint value and its error in dex, and b the discrepancy budget in dex. The simulator's own implausibility replaces E by the run's value and drops both Var and the noise. An output whose emulator variance is not finite and positive at every holdout point is not validated, whatever its standardised errors.

## The noise model

A bin holding N galaxies has a Poisson count, and the emulated quantity is `log10 phi`, which is `log10 N` plus a constant. The spec declares the variance this gives the emulated value:

```text
v(N) = scale * (log10 e)^2 / max(N, 1)        in dex^2, with (log10 e)^2 = 0.18861...
```

with `kind: poisson_log10` and `scale: 1.0` for Poisson noise carried to the logarithm, and the same one-galaxy floor the statistic applies to an empty bin (a zero count is set to one galaxy). The scale is declared, not fitted. Each emulator is fitted with `v(N)` of each training run's own captured count as a known variance at that point. At a prediction point the count is not known in advance, so it is taken at the predicted count, `N_pred = 10^E * BoxSize^3 * w / h^3` for a predicted `log10 phi` of E and a bin width of w dex, and the variance of a new run about the prediction is the emulator's own variance plus `v(N_pred)`. That total is the variance in the standardised errors and in the emulator's implausibility. The simulator's own implausibility carries no noise term.

**The unit of `phi`.** `BoxSize` is in Mpc/h, so `phi = N h^3 / (BoxSize^3 w) = N / ((BoxSize/h)^3 w)` is in physical Mpc^-3 dex^-1, and the table and figure labels read `log10 (Mpc^-3 dex^-1)`. The spec's `density_h_power: 3` brings the constraint table, tabulated in h^3 Mpc^-3 dex^-1, to the same unit; the `mass_h_power: -2` and `mass_shift_dex: -0.26` of the shipped spec bring its masses to the run's h and between initial mass functions.

## What binds a capture: the request hash and the manifests

**The request hash** is the SHA-256 of what defines an evaluation: the spec's `model`, `simulation`, `run_file`, `parameters`, `inert_control`, `statistic` and `constraint` sections as validated (so `10` and `10.0`, or a section's keys written in another order, give the same hash), with the constraint's table path replaced by the SHA-256 of the table's bytes. A mapping inside `run_file.overrides` keeps its order, since Mimic can act on it (it runs `modules.phases` in the order written), so a reordered mapping there gives a new hash. `capture` records it in `context.json`; `select-outputs` and `analyse` recompute it from their own spec and refuse a capture whose request differs, with a message naming both files. It does not cover the sections that choose what to run or how to judge it: `design`, `noise_model`, `outputs`, `discrepancy`, `implausibility`, `criteria` and `emulators`. The design is bound by its own SHA-256 in the capture's manifest. So you can change the thresholds, the budgets, the candidates or the output lists and re-analyse the same capture, while an edit to a range, a parameter, the statistic, the template path or the constraint table makes the capture unusable for that spec.

**The two manifests** are in the format `shasum -a 256 -c` reads from inside their own directory (`<sha256>  <relative path>` per line, sorted by path):

- `runs/manifest.sha256` lists every file under `runs/`, paths relative to `runs/`;
- `manifest.sha256` lists the small files directly in the capture (`context.json`, `design.csv`, `runs.csv`, `outputs.csv`, `bins.csv`) and `runs/manifest.sha256`, so the top level binds the lower one.

Verify a capture in full where it was made:

```bash
cd output/bench/main
shasum -a 256 -c manifest.sha256
(cd runs && shasum -a 256 -c manifest.sha256)
```

`analyse` and `select-outputs` check only the top level: every listed file's SHA-256, taken from the bytes that are parsed, must equal its entry, and a capture file the manifest does not list is refused. So a copy of the small files and `runs/manifest.sha256`, without the per-run files (several gigabytes for a few hundred runs), is a capture the analysis accepts. `analyse` also checks that every `train` and `holdout` row has exactly one record in `runs.csv` of the same set, that every run verified, that the capture's exit status is 0, and that the core outputs are not empty; any failure is a verdict of `error` and nothing is fitted.

## Worked example

The shipped spec asks whether emulators transfer to `sage16` on mini-Millennium, constrained by the Baldry et al. (2008) stellar mass function. Five `sage16` parameters (`SfrEfficiency`, `FeedbackReheatingEpsilon`, `FeedbackEjectionEfficiency`, `ReIncorporationFactor` and `RadioModeEfficiency`) vary over logarithmic ranges around their defaults, with the inert control as a sixth dimension; the statistic is the `StellarMass` mass function at snapshot 63 in 0.1 dex bins. Build Mimic first (see [Setup](#setup)), then, from the repository root:

```bash
# 1. Inspect the design without running anything (rows: pilot 133, train 400, holdout 200).
mimic_venv/bin/python bench/mimic-bench/mimic-bench.py design \
    --spec bench/mimic-bench/specs/sage16-smf.yaml --out output/bench/design.csv

# 2. Pilot capture: the default row, the 32 corners and 100 Latin-hypercube points.
mimic_venv/bin/python bench/mimic-bench/mimic-bench.py capture \
    --spec bench/mimic-bench/specs/sage16-smf.yaml --sets pilot --out output/bench/pilot

# 3. Which bins does the selection rule judge, given the pilot's counts?
mimic_venv/bin/python bench/mimic-bench/mimic-bench.py select-outputs \
    --spec bench/mimic-bench/specs/sage16-smf.yaml --capture output/bench/pilot

# 4. Training and holdout capture (one capture holding both sets).
mimic_venv/bin/python bench/mimic-bench/mimic-bench.py capture \
    --spec bench/mimic-bench/specs/sage16-smf.yaml --sets train,holdout --out output/bench/main

# 5. Fit, judge and write the verdict; the exit status is the verdict (0 pass, 1 fail, 2 error).
mimic_venv/bin/python bench/mimic-bench/mimic-bench.py analyse \
    --spec bench/mimic-bench/specs/sage16-smf.yaml \
    --capture output/bench/main --out output/bench/analysis
echo "verdict exit status: $?"
```

Step 3 prints the lists the spec's `outputs.core` and `outputs.exploratory` should hold; the shipped spec already carries its lists, and for a new spec you paste the printed lists into it before step 5. Editing `outputs` does not change the request hash, so the captures stay valid. Each capture runs `./mimic` once per row, `--workers` at a time; a run that fails verification makes the capture exit 2 with the failure in `runs.csv`, and a rerun needs a new `--out`. Read `output/bench/analysis/verdict.json` for the verdict and every criterion's measured value and threshold, `tables/` for the evidence and `figures/` for the pictures. `output/` is git-ignored.

## Using an external design

`design` writes a table that another sampler or an optimiser can replace. `capture --design <csv>` runs a design made elsewhere, held to the same table the bench writes:

- the columns are exactly those `design` writes: `id`, `set`, every real parameter in spec order, then the inert control;
- each `id` is unique and can name a directory (it matches `[A-Za-z0-9][A-Za-z0-9._-]*`), and each `set` label is non-empty and free-form;
- every value is a finite number inside its parameter's `[low, high]`, the inert control's included.

With `--design`, `--sets` filters the rows by label (every row runs when it is omitted). The capture records the table's source and SHA-256 in `context.json` and its rows in `design.csv`. `analyse` needs rows labelled `train` and rows labelled `holdout`, in any number: a design made outside the spec is analysed as one made from it. `select-outputs` needs the spec's own `pilot` rows (the default row, the corners and the Latin-hypercube points, as `design` writes them) and refuses any other, so make the pilot from the spec.

## A spec for another model package

The code names no model, so a spec for another Mimic model package is a new YAML file, copied from the shipped one. It must declare:

- `model`, `simulation` and `run_file.template`: a run file of that package, with `run_file.overrides` setting at least the snapshot the statistic is taken at. A template that sets `input.first_file` or `input.last_file` must read the simulation's whole file range (from `simulations/<simulation>/simulation_info.yaml`); `capture` refuses a subset before any run, since the statistic's volume is the whole box.
- `parameters`: names under the template's `modules.parameters`, with the template's own value as `default`, and a scale each. Integer switches and the structure of a pipeline stay out of the design vector.
- `statistic.property`: a galaxy property that is mass-valued in that package's output schema (`mass_to_msun` must know its unit label); a property that is not a mass is not supported.
- `constraint`: a table of `log10_mass`, `phi` and `sigma_phi`, and the conversion that brings it to the run's conventions.
- `design`, `noise_model`, `outputs` (empty `core` and `exploratory` lists until a pilot fills them), `discrepancy`, `implausibility`, `criteria` and `emulators`.

Then run the stages above on it. The loader's strictness is the checklist: a wrong or missing key fails with its path before anything runs. Keep the model's names in the spec, the constraint table and this worked example only; the package's independence from any model is tested.

## Controls and tests

Run the suite from the repository root:

```bash
make tests-bench
```

It runs the unittest suite under `bench/mimic-bench/tests/` through `tests/framework/unittest_runner.py`, so each test case emits a Mimic result marker, exactly as `make tests-converter` does; it takes about 5 minutes, is manual and is not part of `make tests` (CI runs it). It uses `mimic_venv/bin/python3` when present. Run it when a change touches `bench/`, the run-file keys the bench writes or verifies, the `RunProperties` or `metadata/version_info.json` fields a run records, the output schema, or `plot/mimic-plot/hdf5_reader.py` or `plot/mimic-plot/output_schema.py`.

Most tests need no Mimic run: they use synthetic captures, and the few that read a Mimic file (a run-file template, the helper modules) read the files of the checkout they live in. **The tests that run `./mimic` skip, with their reason, when it is not built or the simulation's tree files are absent**: the capture tests of `test_capture.py` skip as a class unless `./mimic` is an executable file and every `<tree_name>.<n>` file of the shipped spec's simulation exists. A skip is not a pass for those paths; build Mimic for the spec's model and simulation first to exercise them. A test that needs `shasum` skips if it is not installed.

Three kinds of control run through the same `analyse` stage on synthetic captures whose counts are Poisson draws about a smooth expected count, so that the declared noise model is true of them:

- a **known-good** capture that must pass for both emulators;
- a **known-bad** capture, whose holdout is corrupted by a smooth multiplicative error, that must fail calibration for both;
- **planted defects** on the known-good fits that no verdict may pass: a predictive variance scaled by 0.5 or by 10 must be a `fail`, and a constraint left without its h^3 conversion, or with two distant outputs swapped, must be an `error`. The criteria judge the emulators, not the constraint's labels, so a swap of two adjacent outputs, or a scaling of the emulator's own variance alone where the declared count noise dominates the predictive variance, can still pass; the test documents both. The known-good capture with one run marked unverified must also be an `error`.

The emulators have their own known-good control, a smooth six-input function with five active inputs and one inert one, and a count-noise control that checks the declared Poisson term makes them calibrated across counts. A second-model dry run generates a design and a run file, without running it, from another package's template.

## Limits

- **One kind of statistic.** A mass function of one mass-valued property at one snapshot over every galaxy where that property is positive, with no other selection. A model constrained by clustering, halo occupation or a scaling relation, or by a property that is not a mass, needs another statistic function. Mimic's `hod` package, for example, has no stellar mass.
- **One shape of constraint.** A value with a symmetric error per bin. Asymmetric errors and upper limits are not read.
- **One noise model.** `poisson_log10` is the only kind; a statistic whose noise is not a count's needs another.
- **Continuous parameters only.** A package with nothing continuous to vary has nothing to emulate; one emulator is fitted per structure of a pipeline.
- **One wave.** One pilot, one training set and one holdout set; no refocusing over several waves, no constraint set shared with Mimic's figures and no comparison between model structures.
- **Whole-box runs only.** Every tree file of the simulation, because the statistic's volume is the whole box. `capture` checks the per-file layout only for `lhalo_binary` trees; `consistent_trees_ascii`, `consistent_trees_hdf5` and `horizontal_hdf5` rest on Mimic's reader and the run file's range, and any other `tree_type` (`lhalo_hdf5`, a per-file reader the bench does not check) is refused.
- **No second model has been run.** The tool's independence from the model rests on the absence of model names in the code and on the test that builds a design and a run file for another package without running it.
- **Not bit-for-bit reproducible across machines.** The linear-algebra library can return results that differ in the last bits for identical calls, so a reanalysis is expected to agree with an earlier one to within that, not to the last digit; `context.json` and `verdict.json` record the library versions in use.
- **A verdict is relative to its spec.** A `pass` is relative to the spec's criteria on one holdout; it judges the emulators' variance, not the model's agreement with the data, and a capture is a measurement of one Mimic build (its commit is recorded in `context.json`).

## Module map

All modules are in `bench/mimic-bench/mimic_bench/`; `mimic-bench.py` is a thin entry that calls `mimic_bench.cli.main`.

| Module | Role |
|---|---|
| `cli.py` | The stages' argument parser, exit statuses and error reporting. |
| `spec.py` | Spec loading and strict validation, YAML 1.2 scalars, the noise model's variance. |
| `constraints.py` | The constraint table and its conversion to the comparison space. |
| `design.py` | The design: defaults, corners, Latin-hypercube points; external design tables. |
| `runfile.py` | Run files from the template, the overrides and a design row; the template checks. |
| `capture.py` | Running, verifying and counting evaluations; `context.json` and the CSVs; output selection. |
| `statistics.py` | The statistic's counts per bin, the selection rule, and the import of Mimic's helpers. |
| `provenance.py` | Hashing of the inputs, the request hash, the package and library records. |
| `manifest.py` | The two manifests and the capture's CSV and JSON readers. |
| `emulator.py` | The two emulators (`gaussian_process`, `bayes_linear`) behind one interface. |
| `diagnostics.py` | Predictions with their noise, standardised errors and the per-output criteria. |
| `implausibility.py` | Implausibility, the rejection rule, the false-exclusion rate and the non-implausible fraction. |
| `analyse.py` | The `analyse` stage: capture checks, fits, judgement, verdict and verdict tables. |
| `supplementary.py`, `tables.py`, `figures.py` | The supplementary tables, every table's schema, and the figures drawn from the tables. |

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [docs/USER-GUIDE.md](../../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [docs/STYLE-GUIDE.md](../../docs/STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [plot/mimic-plot/README.md](../../plot/mimic-plot/README.md): plotting manual
- [convert/mimic-convert/README.md](../../convert/mimic-convert/README.md): merger-tree converter manual
- [tests/README.md](../../tests/README.md): test-suite quick reference
