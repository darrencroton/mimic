# Mimic Emulator Implementation Plan (Step 6, the calibrated-variance spike)

**Purpose:** Execute the *First Work* of [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md): measure whether a cheap emulator predicts `sage16`'s response well enough and knows honestly when it cannot, and how much of parameter space one history-matching iteration removes. The deliverable is a verdict of pass, fail or error against criteria frozen here, with a report. The code that produces it is written as the first working version of the instrument, so that a pass leaves something a campaign or the model builder can call, and a fail leaves nothing to unpick in Mimic.

**Status:** Planning contract, revision 2 (2026-11-10: revision 1 was reviewed by an independent two-reviewer panel, and this revision takes its findings). Seven Mode B slices on one feature branch of a new repository outside Mimic; no slice changes a file in Mimic.

**Planning baseline:** Mimic `9ee08e55` on `main` (v1.6), clean tree. Every `path:line` anchor below was read at that baseline, and every measurement under [Frozen Facts and Evidence](#frozen-facts-and-evidence) was taken on 2026-11-10 with a `sage16` / `mini-millennium` binary whose physics and core sources equal the baseline's. PM binds a run to the SHA-256 of this file's bytes.

**Owner:** [Development pathway, step 6](MIMIC-DEVELOPMENT-PATHWAY.md#the-ordered-road). Requirements: [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Future consumers: [`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) (its science gate) and campaigns run under ai-agent-coder's `project-manager` campaign mode. Evidence of record, written by the slices: `reports/SPIKE-REPORT.md` and `evidence/` in the emulator repository.

---

## Decisions for the owner

Each row is a decision this plan takes as frozen unless the owner changes it before the run starts. The recommendation is the plan's text below; an alternative chosen here changes the slices named.

| # | Decision | Recommendation | Alternatives and their cost |
|---|---|---|---|
| 1 | **Scope.** The brief has a spike and, behind it, a five-item gate for the instrument. | **This plan is the spike only: the brief's four *First Work* steps, ending in a verdict.** The brief leaves emulator family and output transformation to be decided from the spike, so the instrument's own plan (constraint set shared with the figures, injected-degeneracy recovery, structure against structure, refocusing over several waves) is written after the verdict, against its evidence. | Plan the instrument now: its slices would be written against an emulator family and a calibration result that do not exist yet, and would be waste if the brief closes. |
| 2 | **Where the code lives.** The brief asks for a spike with no repository change, outside the product tree; Mode B needs a repository to commit to. | **A new standalone repository, `/Users/dcroton/Local/git-repos/mimic-emulator`,** which drives a Mimic checkout through its run-file interface and takes that checkout's path as an argument. Mimic gains this plan and, afterwards, a status line. Where the instrument finally lives is decided at promotion; the package is written to be relocatable (no path assumption, no import of its own repository layout). | A branch of Mimic never merged to `main` (the `sage26/import-tooling` precedent): the Mimic toolchain is at hand, but the branch carries the whole product tree and the brief's "outside the product tree" is met only for `main`. Or `scripts/` on `main`: contradicts the brief. |
| 3 | **Built for its future callers.** The emulator is to become the model builder's science gate and an instrument inside campaigns. | **Five seams, each cheap now and expensive to retrofit** ([Forward Use](#forward-use-campaigns-and-the-model-builder)): one declared spec file with nothing model-specific in code; non-interactive stages whose exit code is the verdict (0 pass, 1 fail, 2 error); a capture stage that runs once, refuses an existing output directory and ends with a SHA-256 manifest, separate from an analysis stage that reads only captured files; a provenance record of what was measured; and controls, a known-good input that passes and a known-bad one that fails. **Not built:** refocusing waves, several models or statistics, a constraint registry, a state format, anything campaign-specific. | Write a throwaway notebook-style spike: quicker by perhaps two slices, and rewritten from nothing at promotion. |
| 4 | **Dependencies.** `mimic_venv` has numpy, scipy, h5py, PyYAML and matplotlib, and no scikit-learn. | **The repository's own virtual environment with scikit-learn added**, pinned in its `requirements.txt` and created in [Run Preparation](#run-preparation). A Gaussian process fitted by a maintained library is worth more here than one environment fewer, because a wrong emulator is the failure this spike exists to detect. | numpy and scipy only, on `mimic_venv`: no second environment anywhere Mimic runs, at the cost of owning and testing a hand-written Gaussian-process fit. Revisit at promotion. |
| 5 | **The design vector.** The brief says about five `sage16` parameters spanning star formation and supernova feedback. | **`SfrEfficiency`, `FeedbackReheatingEpsilon`, `FeedbackEjectionEfficiency`, `ReIncorporationFactor` and `RadioModeEfficiency`, each sampled uniformly in log10 over its default divided and multiplied by five** (the reheating upper bound clipped to 10, the limit `sage_starburst_feedback` accepts), **plus one inert control** that no run file carries. The first four are the efficiencies of star formation, reheating, ejection and reincorporation, which set the mass function below the knee. The fifth acts mainly above the core outputs (Decision 7), so the core outputs should constrain it weakly: a soft known answer beside the inert control's hard one for "which parameter does the data constrain", and the parameter the exploratory outputs respond to. | `StarFormingDiskFactor` in place of the radio-mode efficiency: closer to the brief's words, but its range and corners are unmeasured, so the probe would be repeated. Narrower ranges: fewer collapsed runs, less to rule out. |
| 6 | **Cost model.** The brief planned a reduced snapshot list and a subset of tree files, at hours of machine time. | **The whole box (all eight tree files) and snapshot 63 only.** Measured: a full run takes 2.4 s, set by the tree-file count and not by the snapshot list, so 733 evaluations take about four minutes at eight processes. A subset of files would cut the volume and add shot noise for nothing. | A subset of tree files: seconds saved, and a volume fraction to carry through every number. |
| 7 | **Outputs and the constraint.** | **The z = 0 stellar mass function at the masses of Baldry, Glazebrook & Driver (2008), in the conventions `models/sage16/plots/figures/stellar_mass_function.py` already applies, compared in log10 phi.** It is the one shipped z = 0 constraint whose error column can be used as it stands: the stellar-mass-density compilation needs every snapshot, and the star-formation-density table's two bound columns are not consistently ordered (the upper bound is in one for 19 rows and in the other for 12). The bins that enter the verdict (the core outputs) are chosen by a rule fixed here and applied to a pilot: centre at least 9.0, `sigma_phi < phi`, and at least 10 galaxies in at least 95% of the pilot's Latin-hypercube runs. A probe gives about 16 bins, 9.05 to 10.55; the bins above them empty out in many runs across these ranges, so they are emulated and reported as exploratory and enter no verdict. | Keep every bin with a count floor: the collapsed bins would decide the calibration verdict. Narrow the ranges until the knee stays populated: a different question from the one the brief asks. |
| 8 | **Emulator families.** The brief leaves the choice to the spike. | **Two, compared on the same design: a Bayes linear emulator in the PRISM form (polynomial regression plus a correlated residual; the primary) and a Gaussian process.** An arctan output transformation is measured for calibration only, to answer the brief's Open Question 4. The Bayes linear emulator departs from PRISM where PRISM is known to be over-confident: residual variance with a degrees-of-freedom correction, and coefficient uncertainty carried into the variance by generalised least squares. | The PRISM form alone, as PRISM implements it: one family fewer, and no answer to Open Question 3. |
| 9 | **The pass criteria** (the brief: "calibrated variance, not accuracy"). | **On 200 held-out runs, per output: at least 0.90 of standardised errors within 2, at most 0.05 beyond 3** (the three-sigma bound the implausibility cut rests on)**, and a root-mean-square standardised error of at least 0.5** (a variance overstated more than fourfold is not calibrated). **An emulator configuration is calibrated when at least 0.75 of the core outputs meet all three**; outputs that do not are left out of the implausibility, as is usual. **And its false-exclusion rate is at most 0.05, counted over runs as the rejection rule acts:** among held-out runs the simulator itself keeps, the fraction the emulator rejects. | All outputs must validate: one awkward bin fails the method. Accuracy thresholds: the brief rules them out. Counting false exclusions over run and output pairs: the many pairs far inside the cut dilute the rate, so it can pass while most kept runs are wrongly rejected; it is reported and not gated. |
| 10 | **Discrepancy budget and "removes almost nothing".** | **Scan 0.05, 0.075, 0.1, 0.15, 0.2 and 0.3 dex and gate at 0.1 dex, the total Rodrigues, Vernon & Bower (2017) adopted for this statistic: the emulator must leave at most 0.75 of the prior box non-implausible.** The 0.075 dex of Bower et al. (2010) is their structural term alone, beside separate terms for sampling and for inactive parameters, so 0.075 and 0.05 dex are scanned as tighter than any published total. No budget is defended here for `sage16` in a 62.5 Mpc/h box, whose own sample variance has no term: the budget stays the brief's Open Question 1, the removal criterion is conditional on it, and the report gives the removal at every scanned budget. A probe that classified 60 runs directly, with no emulator, left 0.50 at 0.1 dex, so an honest emulator can remove at most about half the box at the gate budget. | Gate at 0.075 dex: more is removed, on a budget below any published total, which risks a false signal to promote. Require a larger removal: stricter than the brief's closure condition. |
| 11 | **Publication.** The brief's gate asks for the spike to be "complete and published". | **The report and evidence are committed in the emulator repository, which stays local; the verdict and a pointer are recorded in the brief and the pathway after the run** ([After the Run](#after-the-run)). Whether the repository gets a remote is the owner's call at that point. | Push to a remote during the run: PM never pushes. |
| 12 | **Corrections to the brief.** Its *What Mimic Already Provides* section has stale anchors and an over-estimate of cost. | **Corrected alongside this plan, in the same commit:** the three stale line anchors, the cost ("hours" becomes minutes, with the measurement), the third class of observation table, and a pointer to this plan. Its claims about SAGEswarm are left as written: no checkout is on this machine, and the spike does not depend on them. | Leave the brief alone until the verdict. |

---

## Outcome and Limits

**Delivered when the plan closes:**

- A verdict of pass, fail or error on whether emulation with history matching (a Bayes linear emulator or a Gaussian process) transfers to `sage16`, against criteria fixed before any evaluation of record, with the evidence to check it: the design, every evaluation's outputs, the fitted diagnostics and the report.
- Answers from evidence to the brief's Open Questions 3 (emulator family) and 4 (output transformation), and measurements bearing on Open Questions 1 (budget sensitivity) and 6 (what a campaign's state is).
- A small standalone package, `mimic_emulator`, with three stages (`design`, `capture`, `analyse`), a declared spec, tests and two controls, usable against any Mimic checkout.

**Limits, stated so they are not overclaimed:** one model (`sage16`), one simulation (mini-Millennium), one statistic (the z = 0 stellar mass function), one constraint table in the conventions the shipped figure applies (inherited, not re-derived from the paper), one iteration, one machine. The ranges, the output rule and the removal threshold were set after an exploratory probe of 176 runs, none of them reused. The verdict says whether the method is worth an instrument plan; it does not say `sage16` is well-posed, it validates no gate for mechanical use (the brief's Open Question 7 and the model builder's injected-error requirement stand), and it settles no discrepancy budget: its removal result holds only at the budget it is quoted for. A pass by the Gaussian process where the primary fails is a pass, reported as such, with both configurations judged on the same holdout.

---

## Frozen Facts and Evidence

The numbers this plan designs against, each with its source. A slice that finds one wrong stops rather than silently rebasing. All were measured on 2026-11-10 on the Mac Studio, from the Mimic root, in scratch directories, with no tracked file changed.

**The run surface.**

- A `sage16` run on mini-Millennium takes 2.36 to 2.48 s (all eight tree files, eight snapshots, HDF5) and 2.24 s with snapshot 63 alone; one tree file takes 0.30 s whatever is written. Under eight concurrent processes 176 runs took 55.9 s in all (median 2.50 s each). A run with snapshot 63 alone writes about 11 MB.
- The physics path has no random number generator and no threads (`src/` and `models/sage16/` searched). Two runs of one run file give byte-identical binary output and identical HDF5 datasets; HDF5 files differ only in modification-time bytes and `RunProperties/RunEndTime`.
- Concurrent runs with distinct output directories share no written file. Relative paths in a run file resolve against the working directory, so runs are launched from the Mimic root; the run file may live anywhere and the output directory may be absolute.
- The shipped run file `models/sage16/input/sage16_mini-millennium.yaml` has 15 entries under `modules.parameters`, a top-level `SubSteps: 10`, and no `input:` section (the tree range, files 0 to 7, comes from `simulations/mini-millennium/simulation_info.yaml:4-5`). A run file re-emitted by PyYAML (`safe_load`, then `safe_dump(sort_keys=False)`) at the template's values gives snapshot-63 galaxies byte-identical to the template's in all 42 fields, and 36,530 rows.
- Mimic does not reject a misspelled top-level key (a misspelled `SubSteps` silently runs with one substep) or an extra name under `modules.parameters`; a required parameter that is missing, an out-of-range value and a non-numeric value each exit 1 before any tree is read. So a generated run file is trusted only after the run: the master file's `RunProperties/Parameters` dataset records every name and the value text exactly as written (2,640 entries checked against `repr`), and `RunProperties` carries `ModelName`, `SimulationName`, `FirstFile`, `LastFile`, `SubSteps`, `BoxSize` and `Hubble_h` as attributes.
- A successful run exits 0 and writes `metadata/version_info.json` last; a failed one leaves an empty output directory.
- Accepted ranges, from the modules' `init()` (the stricter of two where two modules load one parameter): `SfrEfficiency` [0, 1]; `FeedbackReheatingEpsilon` (0, 10]; `FeedbackEjectionEfficiency` [0, 10]; `ReIncorporationFactor` [0, 10]; `RadioModeEfficiency` [0, 1]. All 32 corners of Decision 5's box ran and exited 0.

**The statistic and the constraint.**

- The shipped figure computes the model's mass as `log10(StellarMass * 1e10 / h)` over `StellarMass > 0`, all galaxy types (`models/sage16/plots/figures/stellar_mass_function.py:98`), and its mass function through `calculate_mass_function(mass, volume, h, 0.1, 8.0, 13.0)` (`plot/mimic-plot/output_utils.py:293`: counts divided by the volume in (Mpc/h)^3 and the bin width, times h^3).
- The Baldry table is a 50-row literal at lines 149 to 203 of that figure (log10 M from 7.05 to 11.95 in steps of 0.1, phi, sigma_phi), converted at lines 206 to 211: mass `log10(10^x / h^2) - 0.26`, phi and its error times h^3. The last three rows have `sigma_phi` equal to `phi`. The same table is copied in `models/sage26` and `models/sham`; Mimic has no shared observations module.
- `plot/mimic-plot/hdf5_reader.py:48` (`read_hdf5_snapshot`) and `plot/mimic-plot/output_schema.py:99` (`mass_to_msun`) are importable by inserting `plot/mimic-plot` in `sys.path`; the loader that knows volume and h lives in the script `mimic-plot.py`, which is not importable by name, so the package reads `BoxSize` and `Hubble_h` from `RunProperties`.

**The response, from a probe of 176 runs over Decision 5's ranges** (the two controls, 32 corners, 60 Latin-hypercube points, and two 41-point scans; the probe is not evidence of record and its runs are not reused).

- The model's counts peak at a bin centre of 8.46 and fall below the data under about 8.5 (resolution); at centres of 9.0 and above the default model is within 0.08 dex of the data in every bin that holds more than 3 galaxies.
- At the default the bins hold 920 galaxies at 9.06, 392 at 10.46, 176 at 10.96, 40 at 11.26 and 3 at 11.46. Across the ranges the massive end empties: 17 of 60 Latin-hypercube runs have no galaxy at 11.26, and 5 of 32 corners have none at 10.76.
- Decision 7's rule keeps 16 bins on the probe (9.05 to 10.55 in the table's masses); with corners and every run required it keeps 13.
- Run-to-run roughness is 2 to 5 galaxies per bin, 0.002 to 0.005 dex in the core bins: five to ten times below the Poisson scale. The response is smooth but not simple: `SfrEfficiency` gives a non-monotonic count at 9.96 and plateaus above 0.1, and `RadioModeEfficiency` changes the bins at 9.96 and below by less than 0.05 dex over its whole range, acting mainly above 10.5.
- Classified directly, with no emulator, by the rejection rule on those 16 bins, 0.22, 0.37, 0.50, 0.63, 0.75 and 0.85 of the 60 Latin-hypercube runs are non-implausible at the six budgets.

**The method, from the sources.** PRISM fits, per output, a polynomial of order 3 in the active parameters chosen by cross-validated stepwise selection, takes the in-sample mean squared error as the residual variance, uses a Gaussian correlation with a fixed length of 0.3 of each range, and inverts with a pseudo-inverse; it has no held-out diagnostic, and added a variance inflation of 0.2 because its variance was found to be underestimated (PRISM source, `prism/emulator/_emulator.py`, v1.3.2; van der Velden et al. 2019, sections 2.3 and 3.3). The Meraxes analysis used 500 evaluations for 9 parameters in its first iteration and reported no held-out test. The validation literature gives the diagnostics used here: individual standardised errors with values beyond 2 marking conflict (Bastos & O'Hagan 2009, section 3.2), and the comparison of emulator with simulator implausibility at validation points, with 0.05 as the yardstick for a wrongly excluded point (Iskauskas et al. 2022, the `hmer` package, section 3.2). Published discrepancy budgets: Bower et al. (2010, section 3.6) took 0.075 dex for the structural term of a luminosity function, beside separate terms for the finite number of sub-volumes (0.014 to 0.022 dex) and for inactive parameters; Rodrigues, Vernon & Bower (2017) adopted 0.1 dex in all for the stellar mass function. Iskauskas et al. add that a wrongly excluded output matters only where the other outputs would keep the point, which is why the false-exclusion rate here is counted over runs. An output that fails validation is left out of that iteration's implausibility (the practice reported for Andrianakis et al. 2015, read here through a summary and not in full).

**Environment.** Python 3.14.6; the pins numpy 2.3.4, scipy 1.18.1, h5py 3.15.1, PyYAML 6.0.3 and matplotlib 3.10.7 are Mimic's own (`requirements.txt`); scikit-learn 1.9.1, pytest 9.2.0 and ruff 0.17.4 resolve against them for this interpreter (a dry-run install, nothing installed). Machine: Mac Studio, 32 cores.

---

## Design

**One spec, three stages.** A campaign spec (YAML) declares what is varied, what is measured, what it is compared with and what counts as a pass. `design` turns the spec into a table of evaluations. `capture` runs them against a Mimic checkout and records, per run, the galaxy count in every candidate bin; it is the only stage that needs Mimic, and it runs once. `analyse` reads a capture's small files and the spec, fits the emulators on the training set, tests them on the holdout, computes implausibility against the constraint and writes the verdict. Every number the report states is in a file a stage wrote or in the pilot's record.

**The evaluation is checked after it runs, not before.** Mimic accepts a run file with a misspelled key, so each run is verified from what it recorded: exit status, the completion marker, and the `RunProperties` attributes and parameter table against the design row.

**The emulated quantity** is `log10 phi` per core output, with a zero count set to one before the logarithm (the single-galaxy floor) and the number of floored values reported. The constraint enters as `log10 phi` with standard deviation `sigma_phi / (phi ln 10)`.

**Implausibility** for output k at point x is `|E_k(x) - z_k| / sqrt(Var_k(x) + sigma_k^2 + b^2)`, with E and Var the emulator's mean and variance, `z_k` and `sigma_k` the constraint and b the discrepancy budget, all in dex. The simulator's own implausibility at a run replaces E with the run's value and drops Var. A point is rejected when the second-largest implausibility over the validated core outputs exceeds 3.

**Pilot before freeze.** Which bins are core is a measurement, so it is taken on a pilot design under a rule fixed here, and the main design is captured only after the rule's result is committed in the spec.

---

## Forward Use: Campaigns and the Model Builder

What the two future callers need, read from campaign mode's contract (`project-manager`, `references/campaign-mode.md`: an instrument "counts only after it passes a known-good control and fails a known-bad one, and its verdict is always pass, fail or error"; a receipt records the measured product SHA, the instrument version, the configuration, the command, the outcome and the artifact hashes; an evidence runner captures once and refuses to run twice) and from the model builder's brief (orchestration state stays out of the product repository; a gate auto-clears only with injected-error evidence):

| Need | How this plan meets it |
|---|---|
| A verdict a runner can record | `analyse` exits 0, 1 or 2 for pass, fail or error and writes the same word in `verdict.json` |
| Trust before use | Two controls ship as tests: a synthetic capture that must pass and a corrupted one that must fail, through the same command |
| Capture once, analyse from files | `capture` refuses an existing output directory and ends with a manifest; `analyse` reads only captured files and checks their hashes |
| The measured product SHA and instrument version | `context.json` records the command line, the Mimic version and commit each run reported, the SHA-256 of the binary and of every Mimic file read, the package version and commit, and the SHA-256 of the spec and of the constraint file |
| Any checkout, including a node worktree | The Mimic root is an argument; nothing is read from `docs/dev/` or written outside the output directory |
| Another model, simulation or statistic later | Model, parameters, ranges, outputs, constraint and thresholds live in the spec; code names none of them |

Not met here, and owed by the instrument's plan: mutation evidence for the gate, a constraint set shared with the figures, refocusing, comparison between structures, and a decision on where the package lives.

---

## Run Preparation

Done once by the owner, or by the PM session at the owner's go-ahead, before `init`. Nothing here is slice work: the dependency manifest and the environment are kept out of every slice's surface.

```bash
# 1. Mimic: a clean build of the commit the run measures (defaults: sage16, mini-millennium)
cd /Users/dcroton/Local/git-repos/mimic
git status --short                 # must print nothing, so the build's version stamp is clean
make clean && make -j"$(sysctl -n hw.ncpu)"
make info                          # confirm model set sage16, simulation mini-millennium

# 2. The emulator repository and its environment
mkdir /Users/dcroton/Local/git-repos/mimic-emulator
cd /Users/dcroton/Local/git-repos/mimic-emulator
git init -b main
python3 -m venv .venv
```

Then write four files, install, and commit. `requirements.txt` (the first five pins are Mimic's own):

```text
numpy==2.3.4
scipy==1.18.1
h5py==3.15.1
PyYAML==6.0.3
matplotlib==3.10.7
scikit-learn==1.9.1
pytest==9.2.0
black==26.5.1
isort==8.0.1
ruff==0.17.4
```

`pyproject.toml` (Mimic's formatter and linter settings, so the lint gate has a configuration to read):

```toml
[tool.black]
line-length = 100

[tool.isort]
profile = "black"
line_length = 100

[tool.ruff]
line-length = 100
target-version = "py312"

[tool.ruff.lint]
select = ["F", "E9", "B", "T10", "DTZ", "PLE"]
ignore = ["F541"]

[tool.pytest.ini_options]
testpaths = ["tests"]
```

`.gitignore`:

```text
.venv/
output/
__pycache__/
.pytest_cache/
.ruff_cache/
.pm
HANDOFF.md
.orchestrator/
```

`README.md`: one paragraph saying what the repository is for, in its own words; it names no planning document.

```bash
.venv/bin/python -m pip install -r requirements.txt
git add requirements.txt pyproject.toml .gitignore README.md
git commit -m "Scaffold: pinned environment and tool configuration"
python3 ~/.claude/skills/lint/scripts/lint.py detect      # resolve anything it reports missing
```

---

## Execution Structure

- **Repository and branch.** The run's repository is `/Users/dcroton/Local/git-repos/mimic-emulator`; the branch is `spike/calibrated-variance`, created at `init`. This plan stays in Mimic and is passed to `init` by its absolute path.
- **Mimic is read and executed, never written.** No slice writes in the Mimic checkout, and every slice ends with `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` printing nothing: those three paths are what the run reads besides the binary. Other work may go on in that checkout during the run, provided nobody rebuilds `./mimic` or changes those paths between Run Preparation and the end of Slice 7; each capture records the SHA-256 of the binary and of every Mimic file it reads, and a difference between the pilot's and the main capture's stops the run. No sandbox policy is used: Developers must execute `./mimic` in that checkout.
- **Checks PM runs per slice.** `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` (no skip is acceptable with `MIMIC_ROOT` set), `.venv/bin/black --check .`, `.venv/bin/isort --check-only .`, `.venv/bin/ruff check .`, and the Mimic status line above. Python style follows Mimic's STYLE-GUIDE (*Python Style Beyond Formatting* and *Python Docstrings*).
- **Measurement protocol.** Evaluations run from the Mimic root with `./mimic -q`, eight at a time, each into its own new directory, each with a 120 s timeout. A capture is made once; a correction that would re-measure stops the slice for the PM. Captures go under the repository's git-ignored `output/`.
- **Stops.** A capture that exits 2, a Mimic version or recorded Mimic SHA-256 that differs between the pilot and the main capture, fewer than 8 core outputs from the pilot, or a frozen fact found wrong, each stop the slice with a question to the PM. A verdict of fail, faithfully reported, is acceptable work.
- **Developer seats** (`Recommended Developer:` per slice; full model ids for the PM):

| Slice | Recommended Developer | Effort | Difficulty | Risk | Reason |
|---|---|---|---|---|---|
| 1 | `claude-sonnet-5-5` | high | moderate | standard | Tightly specified validation and a deterministic design |
| 2 | `claude-opus-5-5` | medium | moderate | standard | Subprocesses, HDF5 verification and units: a silently wrong run is the hazard |
| 3 | `claude-sonnet-5-5` | high | easy | standard | Runs fixed commands and applies a fixed rule |
| 4 | `claude-opus-5-5` | high | hard | elevated | Numerics where a wrong variance is the failure under test |
| 5 | `claude-opus-5-5` | high | hard | elevated | The verdict logic and its controls |
| 6 | `claude-sonnet-5-5` | high | moderate | standard | Reporting analyses and figures over finished numerics |
| 7 | `claude-opus-5-5` | high | moderate | elevated | The measurement of record and its report |

- **Reviewers.** Drift auditor `claude-sonnet-5-5` at high effort; code reviewers `claude-opus-5-5` at high effort and `gpt-6.1-sol` on Codex at high effort (pass `high` explicitly to Codex).

## Resource Profile

Machine time and disk are estimates from the probe's measured rate (2.5 s a run at eight processes, 11 MB a run); Developer time is not estimated, because no slice of this kind has been run.

| Slice | Mimic evaluations | Machine time | Disk |
|---|---|---|---|
| 2 | about 6, in tests | under a minute | about 70 MB, in a temporary directory |
| 3 | 133 | about 45 s | about 1.5 GB |
| 4 to 6 | none | emulator fits in tests: seconds to minutes, not measured | none |
| 7 | 600 | about 3.5 minutes for the capture; the analysis is not measured | about 6.6 GB |

---

## Slice 1: Declared inputs: spec, constraint table and design

### Intended Change
- Create the `mimic_emulator` package with a version string and a command-line entry point (`python -m mimic_emulator <stage>`), and three modules: `spec.py` (load and validate a campaign spec), `constraints.py` (load a constraint table and convert it to the comparison space) and `design.py` (build the design).
- Add the spike's spec, `specs/sage16-smf-spike.yaml`, holding everything the campaign varies or judges by, with the content in the table below.
- Add `constraints/baldry2008_smf.csv`: the Baldry, Glazebrook & Driver (2008) table transcribed unconverted from `models/sage16/plots/figures/stellar_mass_function.py:149-203` in the Mimic checkout (columns log10 M, phi, sigma_phi), with comment lines naming the source file, its line range and the Mimic commit.
- Add the `design` stage: `python -m mimic_emulator design --spec <spec> --out <file.csv>` writes the design table.

Spec content (key names are the Developer's; every item is present and validated):

| Item | Value |
|---|---|
| Model, simulation | `sage16`, `mini-millennium` |
| Run-file template | `models/sage16/input/sage16_mini-millennium.yaml`, relative to the Mimic root |
| Run-file overrides | `output.snapshot_list: [63]` |
| Design parameters, each with low, high and the template's default, sampled uniformly in log10 | `SfrEfficiency` 0.01, 0.25, 0.05; `FeedbackReheatingEpsilon` 0.6, 10.0, 3.0; `FeedbackEjectionEfficiency` 0.06, 1.5, 0.3; `ReIncorporationFactor` 0.03, 0.75, 0.15; `RadioModeEfficiency` 0.016, 0.4, 0.08 |
| Inert control | `InertControl` 0.1, 10, 1.0: sampled like a parameter and marked inert, so no run file carries it |
| Design sets and seed | `pilot`: one row at every default, the 32 corners of the five real parameters (inert control at its default), then 100 Latin-hypercube points; `train`: 400 Latin-hypercube points; `holdout`: 200 Latin-hypercube points; seed 20261110 |
| Output statistic | the z = 0 stellar mass function at snapshot 63, in 0.1 dex bins centred on the constraint's converted masses |
| Output selection rule | bin centre at least 9.0; constraint `sigma_phi < phi`; at least 10 galaxies in at least 95 of the pilot's 100 Latin-hypercube runs. Exploratory outputs: bins that meet the first two conditions, fail the third, and hold at least 10 galaxies in the pilot's default row |
| Core and exploratory outputs | both empty in this slice (Slice 3 fills them) |
| Constraint | `constraints/baldry2008_smf.csv`, converted as mass `x - 2 log10(h) - 0.26` and `phi h^3`, with h taken from the run |
| Discrepancy budgets | 0.05, 0.075, 0.1, 0.15, 0.2 and 0.3 dex; gate budget 0.1 dex |
| Implausibility rule | reject when the second-largest implausibility exceeds 3 |
| Criteria | per output, at least 0.90 of holdout standardised errors within 2, at most 0.05 beyond 3, and a root-mean-square standardised error of at least 0.5; calibrated, at least 0.75 of core outputs validated; false-exclusion rate over runs at most 0.05, measurable only with at least 20 holdout runs kept by the simulator; non-implausible fraction at the gate budget at most 0.75 |

### Acceptance Criteria
- [ ] Loading the spec fails, with a message naming the offending key and exit status 2 from the command line, on each of: an unknown key at any level; an item from the table above that is missing; a range with `low >= high`, a non-positive bound or a default outside the range; a duplicate parameter name; a set size that is not a positive integer; a gate budget that is not one of the scanned budgets; a criterion fraction outside (0, 1]; a minimum of kept runs that is not a positive integer.
- [ ] `design` writes one row per evaluation, with an identifier, its set and every parameter value including the inert control: 133 pilot rows, 400 train rows and 200 holdout rows, values written so that reading them back gives the same floats, and two invocations on one spec write byte-identical files.
- [ ] In every Latin-hypercube set each dimension is stratified in log10 (each of the n equal-probability intervals holds exactly one point) and every value lies strictly inside its range; the three sets come from distinct random streams derived from the one seed; the pilot's first row holds every default, and its next 32 rows are the corners.
- [ ] `constraints.py` returns, for a given h, each point's converted bin centre, `log10(phi h^3)`, the standard deviation in dex `sigma_phi / (phi ln 10)`, and a flag where `sigma_phi >= phi`; at h = 0.73 the first centre equals `7.05 - 2 log10(0.73) - 0.26` and exactly the three highest-mass points are flagged.
- [ ] With `MIMIC_ROOT` set, a test parses the `baldry` array literal out of the figure source and finds the CSV's 50 rows equal to it at float32 precision; with `MIMIC_ROOT` unset that test skips and says why.
- [ ] No module names a model, a parameter or a publication: `grep -rn -i -E 'sage16|SfrEfficiency|baldry' mimic_emulator/` prints nothing.
- Inputs: the Mimic checkout at `/Users/dcroton/Local/git-repos/mimic` (read only); the interpreter `.venv/bin/python`; `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic` exported for the test suite. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: the Mimic checkout, which the slice reads and never writes (`git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` prints nothing).

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/__init__.py` (new file)
  - `mimic_emulator/__main__.py` (new file)
  - `mimic_emulator/spec.py` (new file)
  - `mimic_emulator/constraints.py` (new file)
  - `mimic_emulator/design.py` (new file)
  - `specs/sage16-smf-spike.yaml` (new file)
  - `constraints/baldry2008_smf.csv` (new file)
  - `tests/`
- Functions/classes/components allowed to change: everything in the new files.
- Tests allowed or expected to change: new tests under `tests/` for the spec, the design and the constraint table.

### Explicit Non-Goals
- No run of `./mimic`, no run-file generation, no galaxy output read, no emulator.
- No constraint other than the one table, and no edit to it beyond transcription; its h and IMF conventions are applied as the figure applies them and are not re-derived.
- No change to `requirements.txt`, `pyproject.toml` or anything in the Mimic checkout.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: spec validation (one case per rejection named above, and the shipped spec loads); design sizes, stratification, determinism and corner rows; constraint conversion and the transcription check.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/python -m mimic_emulator design --spec specs/sage16-smf-spike.yaml --out output/design-check.csv` twice with a byte comparison; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: the spec reads top to bottom as a statement of the campaign, with units in its comments.

### Rollback Path
- Revert the slice's commit; nothing outside the repository was written.

---

## Slice 2: Evaluation: run files, capture, verification and the statistic

### Intended Change
- Add `runfile.py`: build a run file from a template and one design row by loading the template with PyYAML, setting `output.output_directory` to an absolute per-run directory, applying the spec's overrides and the row's real parameters under `modules.parameters`, and emitting it with key order kept.
- Add `statistics.py`: galaxy counts per candidate bin for one finished run, and the output selection rule as a pure function of a capture's counts.
- Add `capture.py` and the `capture` stage: `python -m mimic_emulator capture --spec <spec> --mimic-root <dir> --sets <names> --out <dir> [--workers 8]` runs every evaluation of the named sets, verifies each, records its counts, and ends with a manifest.
- Add the `select-outputs` stage: `python -m mimic_emulator select-outputs --spec <spec> --capture <dir>` prints the core and exploratory outputs the spec's rule gives for a pilot capture, with the per-bin counts behind them.

### Acceptance Criteria
- [ ] A generated run file, parsed by PyYAML, equals the parsed template except in `output.output_directory`, the spec's overrides and the design row's real parameters; the inert control never appears in it; a design parameter that the template's `modules.parameters` lacks, or a spec default that differs from the template's value, stops `capture` with exit status 2 before any run.
- [ ] `capture` runs each evaluation as `./mimic -q <run file>` with the Mimic root as working directory, in its own new directory under `<out>/runs/`, with stdout and stderr in a per-run log and a 120 s timeout; it refuses an `--out` that exists, and writes nothing outside `--out`.
- [ ] A run counts as verified only when all hold: exit status 0; `metadata/version_info.json` exists; the master file's `RunProperties/Parameters` has exactly the template's parameter names in order, each value parsing to the float the run file carried; `ModelName` and `SimulationName` equal the spec's; `FirstFile` and `LastFile` equal the simulation's full range in `simulations/<simulation>/simulation_info.yaml`; `SubSteps` equals the template's; snapshot 63 is present. A test shows verification failing when the expected row differs from what ran.
- [ ] Counts are over galaxies with `StellarMass > 0` of every type, with mass `log10` of `StellarMass` converted to solar masses through Mimic's own `output_schema.mass_to_msun` and the run's schema unit label, read through `hdf5_reader.read_hdf5_snapshot`, both imported from `<mimic-root>/plot/mimic-plot` with bytecode writing disabled, so that nothing is written in the checkout; the candidate bins are every constraint point's converted centre plus and minus 0.05 dex, with h from the run.
- [ ] Known answers, run against the real executable: the evaluation at every default has 36,530 rows at snapshot 63, and 920 and 392 galaxies in the candidate bins of the constraint's 9.05 and 10.45 rows (measured by the probe before the plan); its counts on the edges `numpy.linspace(8.0, 13.0, 51)`, turned into `N h^3 / (BoxSize^3 * 0.1)`, equal Mimic's `calculate_mass_function(mass, BoxSize**3, h, 0.1, 8.0, 13.0)` to 1e-12 relative; and two evaluations of one design row give identical counts.
- [ ] `<out>` holds `context.json` (the command line, package version and commit, spec path and SHA-256, the constraint file's SHA-256, Mimic root, the Mimic version and commit the runs reported, the `BoxSize` and `Hubble_h` they reported, the SHA-256 of `./mimic`, of the template and of every other Mimic file `capture` reads (the simulation's `simulation_info.yaml` and the two imported helpers), host, start and end times), `design.csv`, `runs.csv` (per run: set, exit status, wall time, verified, rows at snapshot 63), `outputs.csv` (per run: the count in every candidate bin) and `manifest.sha256` listing every other file under `<out>`; `shasum -a 256 -c manifest.sha256` passes.
- [ ] `capture` exits 0 only when every evaluation verified and all reported one Mimic version and commit and one `BoxSize` and `Hubble_h`; otherwise it still writes every file, records the failures in `runs.csv`, and exits 2.
- [ ] `select-outputs` applies the spec's rule exactly (a test on synthetic counts covers a bin kept, a bin dropped by each of the three conditions, and an exploratory bin) and changes no file.
- Inputs: the Mimic checkout at `/Users/dcroton/Local/git-repos/mimic`, built by run preparation (read and execute only); the interpreter `.venv/bin/python`; `MIMIC_ROOT` exported as that path; Slice 1's spec, constraint table and design. Tests that run the executable use a temporary directory and a design of at most six rows. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: Slice 1's modules and their tests; the Mimic checkout, which the slice reads and never writes (`git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` prints nothing).

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/__main__.py`
  - `mimic_emulator/runfile.py` (new file)
  - `mimic_emulator/statistics.py` (new file)
  - `mimic_emulator/capture.py` (new file)
  - `tests/`
- Functions/classes/components allowed to change: the new modules; in `__main__.py`, the two new stages only.
- Tests allowed or expected to change: new tests under `tests/`; those that need the executable are marked and skip, with the reason, only when `MIMIC_ROOT` is unset.

### Explicit Non-Goals
- No pilot or main capture (Slices 3 and 7), no emulator, no implausibility.
- No subset of tree files, no snapshot other than the spec's, no binary output format, no `--skip` or resume: a capture is whole or it is an error.
- No reimplementation of Mimic's reader or unit conversion, and no import of `mimic-plot.py`.
- No deletion of run output.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, medium effort.

### Validation Plan
- Tests to add/update: run-file generation against the shipped template; verification, positive and negative; the statistic's known answers; capture end to end on a small design, including the refusal of an existing directory and the manifest; the selection rule.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` (no skips); `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`; `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot`
- Lint (differential, via the `lint` skill): required.
- Manual checks: print one generated run file beside the template and read the differing keys.

### Rollback Path
- Revert the slice's commit; test captures live in temporary directories.

---

## Slice 3: Pilot capture and the frozen outputs

### Intended Change
- Capture the pilot set once (`--sets pilot --out output/pilot`), apply the output selection rule with `select-outputs`, and write the resulting core and exploratory outputs into `specs/sage16-smf-spike.yaml`.
- Commit the capture's small files under `evidence/pilot/` and a short record, `reports/PILOT-RECORD.md`.

### Acceptance Criteria
- [ ] The pilot capture ran once from a clean worktree and exited 0 with 133 of 133 runs verified, every corner among them; the default row has 36,530 rows at snapshot 63; `shasum -a 256 -c manifest.sha256` passes in `output/pilot`.
- [ ] The spec's core and exploratory outputs equal what `select-outputs` prints for that capture, and `git diff` of the spec shows no other change.
- [ ] The slice stops with a question to the PM, freezing nothing, if the rule gives fewer than 8 core outputs or core outputs that are not one contiguous run of bins.
- [ ] `evidence/pilot/` holds `context.json`, `design.csv`, `runs.csv`, `outputs.csv` and `manifest.sha256`, byte-identical to the capture's.
- [ ] The record states the Mimic version and commit, the package commit, the spec's SHA-256 before and after the freeze, the capture's wall time and disk use, and one table with, per candidate bin from 8.5 up: the constraint's `log10 phi` and error in dex, the default row's count, the minimum and the number below 10 over the 100 Latin-hypercube runs and over the 32 corners, and whether the bin is core, exploratory or neither and by which condition.
- Inputs: the Mimic checkout at `/Users/dcroton/Local/git-repos/mimic`, built by run preparation (read and execute only); the interpreter `.venv/bin/python`; the stages from Slices 1 and 2. A probe before the plan gave 16 core bins; that is context, not a requirement.
- Behaviour that must not change: every file outside this slice's surface; the Mimic checkout, which the slice reads and never writes (`git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` prints nothing).

### Authorized Surface
- Files allowed to change:
  - `specs/sage16-smf-spike.yaml`
  - `evidence/pilot/`
  - `reports/PILOT-RECORD.md` (new file)
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No code change, no change to the rule, the ranges, the budgets or the criteria, and no second pilot: a pilot that fails to capture, or whose result looks wrong, is a question to the PM.
- No emulator fit on the pilot and no reading of the pilot against the constraint beyond the table above.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: easy
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: `.venv/bin/python -m mimic_emulator capture --spec specs/sage16-smf-spike.yaml --mimic-root /Users/dcroton/Local/git-repos/mimic --sets pilot --out output/pilot`; `.venv/bin/python -m mimic_emulator select-outputs --spec specs/sage16-smf-spike.yaml --capture output/pilot`; `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` after the freeze; `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot`
- Lint (differential, via the `lint` skill): not required (no linted file changes).
- Manual checks: the record's table against `evidence/pilot/outputs.csv` for three bins chosen by the PM.

### Rollback Path
- Revert the slice's commit and move `output/pilot` aside; the spec returns to its unfrozen state.

---

## Slice 4: The two emulators

### Intended Change
- Add `emulator.py` with two emulators of one scalar output over inputs scaled to the unit cube, behind one interface: `fit(x, y)` on training data, and `predict(x)` returning a mean and a variance, the variance being that of a new simulator evaluation at x (so it includes the nugget or noise term).
- **Bayes linear.** A regression on polynomial terms up to total order 3 in the inputs, the terms chosen by forward stepwise selection scored by cross-validation inside the training set, plus a residual with Gaussian correlation over the active inputs (those in any chosen term) and a nugget. Coefficients are the generalised least squares estimate under that correlation; the residual variance is the generalised residual sum of squares over n minus the number of terms; the predictive variance carries the coefficient uncertainty (the universal-kriging form). The correlation length and nugget fraction are chosen from a small fixed grid by cross-validated Gaussian log predictive density inside the training set.
- **Gaussian process.** scikit-learn's `GaussianProcessRegressor` with a constant times an anisotropic squared-exponential kernel plus a white-noise kernel, on a standardised output, hyperparameters by maximum likelihood with restarts from a fixed seed; the predictive variance includes the fitted noise.

### Acceptance Criteria
- [ ] Both emulators expose the same `fit` and `predict`, report their active inputs or length scales after fitting, read no data but what `fit` is given, and give identical predictions from two fits on the same data.
- [ ] Known-good control: on a smooth six-input synthetic function that is not a polynomial of order 3 or less, with five active inputs, one inert input and additive noise of 0.5% of the function's standard deviation, fitted on 400 Latin-hypercube points and tested on 200 others, each emulator has at least 0.90 of standardised errors within 2, at most 0.05 beyond 3, a root-mean-square standardised error between 0.5 and 1.5, and a root-mean-square error under 0.2 of the function's standard deviation.
- [ ] On that control the Bayes linear emulator's active inputs exclude the inert input, and the Gaussian process's fitted length scale for the inert input is its largest.
- [ ] The Bayes linear predictive variance is checked against an independent computation: on a fixed small case (at most 30 points, fixed terms, correlation length and nugget) the mean and variance equal a direct dense-matrix evaluation of the universal-kriging formulas written in the test, to 1e-8 relative.
- [ ] At a training point the predicted variance is at least the nugget or noise variance and never negative anywhere; a fit on fewer points than candidate terms, or on a constant output, either succeeds with finite predictions or raises an error that names the cause.
- [ ] The departures from PRISM (degrees-of-freedom correction, generalised least squares coefficient uncertainty, cross-validated correlation length and nugget) are stated in the module docstring with the reason for each.
- Inputs: inputs lie in the unit cube and outputs are finite; behaviour outside that is unspecified. Sizes up to 400 training points, 6 inputs and 30 outputs fitted one at a time. The interpreter `.venv/bin/python`. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: the modules and tests of Slices 1 to 3.

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/emulator.py` (new file)
  - `tests/`
- Functions/classes/components allowed to change: everything in the new module.
- Tests allowed or expected to change: new tests under `tests/` for the two emulators.

### Explicit Non-Goals
- No reading of a capture, no constraint, no implausibility, no verdict, no figure (Slices 5 and 6).
- No multi-output or joint covariance across outputs, no refocusing, no third emulator family, no variance-inflation factor, and no tuning against anything but the training data.
- No PRISM code and no dependency beyond `requirements.txt`.

### Risk Flags
- Risky surfaces touched: numerics (a predictive variance whose honesty is the quantity the spike measures)
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: the known-good control for both emulators; the dense-matrix check; determinism; the edge cases named above.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: record the emulator tests' wall time in `validation.md`.

### Rollback Path
- Revert the slice's commit.

---

## Slice 5: Diagnostics, implausibility and the verdict

### Intended Change
- Add `diagnostics.py` (standardised errors on a holdout and the per-output and per-configuration calibration results), `implausibility.py` (emulator and simulator implausibility, the rejection rule, the non-implausible fraction of the prior box) and `analyse.py` with the `analyse` stage: `python -m mimic_emulator analyse --spec <spec> --capture <dir> --out <dir>`.
- `analyse` forms `log10 phi` per core output from the captured counts (a zero count set to one first), with `BoxSize` and `Hubble_h` taken from the capture's `context.json`, fits each candidate configuration on the `train` set, tests it on the `holdout` set, and writes `verdict.json` and tables. The candidate configurations, in order, are the Bayes linear emulator and the Gaussian process, both on `log10 phi`.
- The functions that compute a criterion or the verdict are pure functions of arrays and the spec, separate from the stage function that reads and writes files, so a later slice can add outputs without touching them.

### Acceptance Criteria
- [ ] Per configuration and core output, `analyse` reports on the holdout: the fractions of standardised errors within 1, 2 and 3, their mean and root mean square, the root-mean-square error in dex, and `validated` when the spec's three per-output criteria hold; an output whose predicted variance is non-finite or not positive at any holdout point is not validated; a configuration is `calibrated` when the spec's fraction of core outputs is validated.
- [ ] Implausibility follows the plan's definition exactly: for output k, `|E - z| / sqrt(Var + sigma^2 + b^2)` in dex with the constraint's converted value and error; the simulator's replaces E by the run's own value and drops Var; a point is rejected when the second-largest value over the configuration's validated core outputs exceeds 3. A test checks one hand-computed case of each. A configuration with fewer than two validated core outputs has no implausibility computed and cannot pass.
- [ ] The false-exclusion rate, at each budget, is counted over runs: among holdout runs the simulator keeps under the rejection rule on the configuration's validated outputs, the fraction the emulator rejects; `verdict.json` records its numerator and denominator. A test on constructed implausibilities has a rate over run and output pairs under 0.05 and a rate over runs above it, and the criterion fails. Reported beside the rate, and entering no criterion: the rate over run and output pairs, and the fraction of simulator-rejected holdout runs that the emulator also rejects.
- [ ] The non-implausible fraction, at each budget, is taken over 100,000 Latin-hypercube points of the whole design box, sampled as the design is (uniform in log10) from a fixed seed; beside it `analyse` reports the fraction of the 200 holdout runs the simulator keeps on the same validated outputs, with a binomial standard error.
- [ ] A configuration passes when it is calibrated, its false-exclusion rate at the gate budget is at most the spec's limit, and its non-implausible fraction at the gate budget is at most the spec's limit. The verdict is `pass` when at least one candidate passes, naming the first that does in candidate order and saying when it is not the primary; `fail` when none does; `error` when the capture has an unverified run, the SHA-256 of a file `analyse` reads differs from its manifest entry (entries for files it does not read are ignored), the spec's or the constraint file's SHA-256 differs from the one the capture recorded, the core outputs are empty, or the simulator keeps fewer holdout runs at the gate budget than the spec's minimum for a calibrated configuration (its false-exclusion rate cannot be measured).
- [ ] `analyse` exits 0, 1 or 2 for pass, fail or error, and `verdict.json` holds the same word, every criterion's measured value and threshold per configuration, the count of floored values, and the SHA-256 of the spec, the capture's manifest and the package commit.
- [ ] Controls, run through the `analyse` stage on synthetic captures built in the test (no Mimic): a known-good capture, whose outputs are a smooth function of the design with a constraint placed inside its range, exits 0; a known-bad capture, the same but with the holdout rows' outputs generated from a different function, exits 1 with the calibration criterion failing; a capture with one run marked unverified exits 2.
- [ ] `analyse` reads only `context.json`, `design.csv`, `runs.csv`, `outputs.csv` and `manifest.sha256` from the capture, plus the spec and its constraint file, and writes only under `--out`, which it refuses if it exists.
- Inputs: a capture written by Slice 2's `capture` (or a synthetic one of the same shape); the frozen spec; Slice 4's emulators through their interface. The interpreter `.venv/bin/python`. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: the modules and tests of Slices 1 to 4.

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/__main__.py`
  - `mimic_emulator/diagnostics.py` (new file)
  - `mimic_emulator/implausibility.py` (new file)
  - `mimic_emulator/analyse.py` (new file)
  - `tests/`
- Functions/classes/components allowed to change: the new modules; in `__main__.py`, the new stage only.
- Tests allowed or expected to change: new tests under `tests/`, including the three controls.

### Explicit Non-Goals
- No change to `emulator.py`: a defect found there is a question to the PM.
- No run of `./mimic`, no analysis of the pilot or main capture (Slice 7), no figures or supplementary analyses (Slice 6).
- No threshold, budget or rule outside the spec, no multivariate implausibility, no second wave.

### Risk Flags
- Risky surfaces touched: verdict logic (the outcome a campaign or the owner will act on)
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: diagnostics on arrays with known fractions; the hand-computed implausibility cases; the rejection rule with an unvalidated output excluded; the constructed false-exclusion case; the degenerate cases (fewer than two validated outputs, too few kept runs, a non-positive variance); the three controls; the refusals.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read `verdict.json` from the known-good control and confirm every criterion in the spec appears with its value.

### Rollback Path
- Revert the slice's commit.

---

## Slice 6: Supplementary analyses and figures

### Intended Change
- Extend `analyse` to write, beside the verdict and changing nothing in it, four supplementary results, each as a table under `<out>/tables/`:
  - a learning curve: the Slice 5 per-output diagnostics for each candidate refitted on 100, 200 and 400 training rows (nested fixed-seed subsets);
  - the transformation comparison: the Bayes linear emulator fitted on `arctan(log10 phi)`, with its calibration fractions in the transformed space and its error mapped back to dex;
  - the exploratory outputs: the Slice 5 per-output diagnostics for the spec's exploratory outputs, marked as entering no criterion;
  - per-parameter profiles: for the passing configuration (or the primary when none passes), the non-implausible fraction at the gate budget in 20 equal slices of each design dimension, the inert control included (reported as not computed when that configuration has no implausibility).
- Add `figures.py`: standardised-error distributions and predicted-against-run values per configuration, the non-implausible fraction against budget with the simulator's own estimate, the learning curve, and the profiles, written as PNG files under `<out>/figures/`.

### Acceptance Criteria
- [ ] The verdict path is unchanged: Slice 5's tests pass without modification, and the slice changes no line that computes a criterion or the verdict; the stage function may gain calls made after `verdict.json` is written.
- [ ] Each of the four tables is written with a header naming its quantities and units, and each figure is drawn from a table, never from a second computation.
- [ ] On the known-good control the inert control's profile is flat (every slice within 0.05 of its mean) and at least one active input's profile spans more than 0.2.
- [ ] The learning curve uses nested subsets (each smaller set inside the next) and reports the configuration's `calibrated` result and median root-mean-square error at each size; a size at which a fit raises is reported as not fitted, with the cause.
- [ ] Figures render with the non-interactive Agg backend and with `MPLCONFIGDIR` set to a directory under `--out`, their axis labels carry units, and the stage writes nothing outside `--out`.
- Inputs: Slice 5's `analyse` and its outputs; Slice 4's emulators through their interface. The interpreter `.venv/bin/python`. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: every criterion, threshold and verdict of Slice 5; the modules and tests of Slices 1 to 4.

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/analyse.py`
  - `mimic_emulator/figures.py` (new file)
  - `tests/`
- Functions/classes/components allowed to change: in `analyse.py`, additions that write supplementary tables and call `figures.py`; the verdict path is not edited.
- Tests allowed or expected to change: new tests under `tests/` for the four tables and a smoke test that the figures are written.

### Explicit Non-Goals
- No new criterion, no change to how a criterion is computed, and no supplementary result feeding the verdict.
- No interactive plotting, no projection figures beyond the one-dimensional profiles, no notebook.
- No change to `emulator.py`, `diagnostics.py` or `implausibility.py`: a defect found there is a question to the PM.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the unchanged-verdict check; the profile known answer; nested subsets; table headers; the figure smoke test.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: open each figure from the known-good control once.

### Rollback Path
- Revert the slice's commit; Slice 5's stage is unchanged underneath.

---

## Slice 7: The main capture, the analysis and the report

### Intended Change
- Capture the `train` and `holdout` sets once (`--sets train,holdout --out output/main`), commit the capture's small files under `evidence/main/`, run `analyse` on that committed copy into `evidence/main/analysis/`, and write `reports/SPIKE-REPORT.md`.
- Complete `README.md`: what the package is, its stages and exit statuses, the spec, how to reproduce the analysis from `evidence/` without Mimic, and what a caller must supply to run it as an evidence node.

### Acceptance Criteria
- [ ] The main capture ran once from a clean worktree and exited 0 with 600 of 600 runs verified; its Mimic version and commit and every Mimic SHA-256 in its `context.json` equal the pilot's in `evidence/pilot/context.json`; `shasum -a 256 -c manifest.sha256` passes in `output/main`; `evidence/main/` holds the five small files byte-identical to the capture's.
- [ ] `analyse` ran once on `evidence/main/` with the frozen spec and exited 0 or 1; the report's verdict line equals `verdict.json`, and an exit of 2 stops the slice with a question to the PM.
- [ ] Every number in the report is quoted from a named file under `evidence/` or from `reports/PILOT-RECORD.md`, and every table in the report names the file it was copied from.
- [ ] The report has, in order: the verdict and what it supports (writing a plan for the instrument, or stopping); what was measured (Mimic version and commit, package commit, spec SHA-256, design sizes, wall time); the frozen outputs, from the pilot record; calibration per configuration and output; the false-exclusion rate with its numerator and denominator, and the emulator's power against the simulator's own classification; the non-implausible fraction at every scanned budget, beside the simulator's estimate, with how far the removal criterion was from binding; the profiles, with the inert control's result stated; the learning curve; the comparison of the two emulator families and the comparison of output transformations, each with the evidence and a recommendation; the exploratory outputs; observations on how the result depends on the discrepancy budget and on what a campaign's stored state consists of, labelled as observations; limits; and what a plan for the instrument must take from this.
- [ ] The limits section states: one model, simulation, statistic, constraint, iteration and machine; that candidate configurations were judged on one holdout; that the ranges, the output rule and the removal threshold were set after an exploratory probe; that the discrepancy budget is not defended for this model and box and carries no term for the box's own sample variance, so the removal result is conditional on it; that the constraint's h and IMF conventions are the shipped figure's and were not re-derived; that the verdict validates no gate for mechanical use; and every floored value and unvalidated output by name.
- [ ] The report and the README say what they answer in their own words and cite no document under Mimic's `docs/dev/`; references to Mimic are to its committed code, by path and commit.
- [ ] A verdict of fail is reported as plainly as a pass, with the criterion that failed and its measured value; nothing in the spec, the code or the evidence is changed to alter a result.
- [ ] After review, a correction edits the report or the README only, from the files already captured; a correction that would need a new capture or a new analysis stops the slice for the PM.
- Inputs: the Mimic checkout at `/Users/dcroton/Local/git-repos/mimic`, built by run preparation, with its binary and the files the run reads unchanged since the pilot (read and execute only); the interpreter `.venv/bin/python`; the frozen spec; `evidence/pilot/` and `reports/PILOT-RECORD.md`; the stages from Slices 1 to 6.
- Behaviour that must not change: every file outside this slice's surface; the Mimic checkout, which the slice reads and never writes (`git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` prints nothing).

### Authorized Surface
- Files allowed to change:
  - `evidence/main/`
  - `reports/SPIKE-REPORT.md` (new file)
  - `README.md`
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No code or spec change, no second capture, no second analysis run with other settings, no result chosen after seeing it.
- No edit to the Mimic checkout, including its `docs/dev/`: recording the verdict there is the owner's step after the run.
- No claim beyond the limits above, and no decision on the questions it reports on: the report recommends, the owner decides.

### Risk Flags
- Risky surfaces touched: measurement of record (the capture and report the brief's promotion or closure rests on)
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: `.venv/bin/python -m mimic_emulator capture --spec specs/sage16-smf-spike.yaml --mimic-root /Users/dcroton/Local/git-repos/mimic --sets train,holdout --out output/main`; `.venv/bin/python -m mimic_emulator analyse --spec specs/sage16-smf-spike.yaml --capture evidence/main --out evidence/main/analysis`; `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot`
- Lint (differential, via the `lint` skill): not required (no linted file changes).
- Manual checks: an independent read-only trace, by a Reviewer, of every number in the report to its file under `evidence/` or to `reports/PILOT-RECORD.md`.

### Rollback Path
- Revert the slice's commit and move `output/main` aside; the code and the frozen spec are untouched.

---

## After the Run

Owner's steps in Mimic, outside the plan's slices:

1. Read `reports/SPIKE-REPORT.md` and decide: promote the brief to an instrument plan, or close it, as its *First Work* section says.
2. Record the verdict, the measured criteria and a pointer to the emulator repository's commit in [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md) and in the pathway's step 6 row and changelog; move this plan to `archive/dev-plans/`, replacing the links to it in the brief's status line and in the pathway's step 6 row and documents table. The report's two comparisons answer the brief's Open Questions 3 and 4, and its observations on the budget and on stored state bear on Open Questions 1 and 6.
3. If promoted, the instrument's plan starts from the report's last section and owes the brief's gate items 2 to 5, the items under [Forward Use](#forward-use-campaigns-and-the-model-builder) marked not met, and a decision on Decision 2's and Decision 4's alternatives.

---

## Next Chat Prompts

### Mode A — Human-managed plan

```md
Plan file: /Users/dcroton/Local/git-repos/mimic/docs/dev/MIMIC-EMULATOR-IMPLEMENTATION-PLAN.md
Scope: Slice 1
Supervision: checkpointed — I am present: ask me before coding an approval-flagged slice and before each commit
Reviewers (optional): drift auditor claude claude-sonnet-5-5 high; code reviewers claude claude-opus-5-5 high; codex gpt-6.1-sol high

Read the full plan file first. If a slice or batch receipt in scope is incomplete or the plan state is unclear, stop and tell me before coding.

The work happens in /Users/dcroton/Local/git-repos/mimic-emulator, not in Mimic. Confirm the plan's Run Preparation is complete before anything else. Work on the branch spike/calibrated-variance there; if it does not exist, create it and tell me.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer (the reviewers above, when given) only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass. If no Reviewer is configured or available, perform Developer self-audit on a default slice and record that provenance explicitly; for a slice whose Risk Flags say `Independent audit required: yes`, stop and report instead of self-auditing.

For each slice or batch in scope, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If any included slice's Risk Flags mark approval-needed, stop before coding: checkpointed, get my approval; autonomous, end the run and report — never self-approve a slice the plan gated for a human.
3. apply the scoped-implementation skill against the selected contract.
4. apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit. Report the authorization gate result and who performed it before any quality review.
5. If the gate fails, fix the drift inside the contract and re-audit; if it cannot be fixed inside the contract, stop and report.
6. On a passing gate: for a broad or structural change, first run the code-health skill differentially against the slice's starting commit and supply its report as review evidence. Then apply the code-review skill using the code reviewers when available, in parallel if there are several; otherwise perform Developer self-audit through the code-review skill. Record who performed it.
7. Collect every finding from the current audit or review round before fixing (checkpointed, surface them to me first), record them in the slice summary, fix them as one batch, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged, record residuals in the slice summary and proceed.
8. Commit with the commit skill: checkpointed, ask me first; autonomous, commit a slice that has passed validation, the drift-audit gate and the code-review gate — and only such a slice.

Stop early on: a blocker, an unapproved scope change, a gate or validation failure unfixable inside the contract, or context pressure. When the scope is done, or on any stop with slices remaining, use the handoff skill to record state, audit provenance (Reviewer tool/label or Developer self-audit and fallback context), and the next slice or batch to resume from. Do not continue past the scope.

When the scope was all remaining slices and the plan is complete, use the report skill instead to write a final report covering slices committed, gate results and audit provenance per slice, validation, and every residual or post-plan consideration left for me to assess. Do not lose a non-blocking observation merely because it did not belong in the frozen plan.

Confirm before starting: plan file read, scope, supervision, branch, and the first slice. Then begin.
```

### Mode B — PM-managed plan

```md
Plan file: /Users/dcroton/Local/git-repos/mimic/docs/dev/MIMIC-EMULATOR-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic-emulator
Developer: claude per plan (each slice's Recommended Developer line)
Drift auditor: claude claude-sonnet-5-5 high
Code reviewers: claude claude-opus-5-5 high; codex gpt-6.1-sol high
Run tag (optional): emulator-spike
Use the project-manager skill. You are the PM: the accountable supervisor of this run. You never write slice code.

Handoff: HANDOFF.md in the repo root, git-ignored (add it to .git/info/exclude if it is not; a campaign child's is already excluded), is the sole entry point for a restart and for my post-run review. Create it after init and update it after every slice decision and before any stop with: run identity (never the token), seats, branch, commit authorisation, review protocol, slice log, plan decisions, residuals and follow-ups, and PM lessons.

Starting the run: `init` takes full model ids, efforts where the harness supports them, the run tag and, when a sandbox policy is given, `--sandbox <path>`; set `PM_TMUX_SOCKET` exactly as the `export PM_TMUX_SOCKET=…` line init prints, for every later command of the run.

Human present (the default):
- If HANDOFF.md names an unfinished run of this plan, read it fully and resume that run (I will supply the token). Otherwise prepare, then confirm with me once:
  - Read the whole plan, run check-plan with repo context, and check that the repo still matches what the plan assumes. Init freezes the plan, so raise any drift or warning now.
  - The plan is committed in the Mimic repository, not in the run's repository, which meets the plan-committed requirement. The plan's Run Preparation must be complete before init: check it, and if it is not, do it only at my go-ahead.
  - Require a clean worktree in the run's repository. Propose the plan's feature branch, spike/calibrated-variance, to create.
  - Confirm: plan read, check-plan result, seats, branch, first slice, any approval-flagged slices (each stops the run until I approve it), my standing authorisation for Developers to commit each slice, and, when the Developer's harness asks for directory trust per worktree, my permission to add the repo's trust entry yourself (project-manager's references/sandbox.md *Troubleshooting*).
- After my go-ahead, run init. Give me the run id and token, then begin.

Per slice, in plan order:
1. start-slice with the Developer seat's model and effort. Wait with one long background `observe --wait`; never poll.
2. Run finalize, then your own checks: pane, diff, validation output, full test suite, lint, differential code-health if the structure changed, and a style-guide audit (the project's guide, otherwise the baseline). Treat newly introduced, in-contract style deviations as cleanup to steer.
3. Review as project-manager's *Risk* section sets: on a standard slice your own checks in step 2 are the review, and commissioning more is your recorded review-depth discretion; an elevated slice (`Independent audit required: yes` or a risky surface) commissions the drift audit first, which you read and rate, then the code reviewers, who share one `--adjudicated-file`. Let reviews run to completion without watching them.
4. Rate every commissioned report, order the code panel when there is one, and score the Developer (criteria met, confirmed defects).
5. Decide: accept, steer or stop. Steer only once every review is in, with all findings batched into one correction. Each follow-up round re-runs what the change needs: on an elevated slice, both reviews fresh at the new head, focused on the change since the last round through an adjudication line.
6. Update `pm notes` for the next Developer, and HANDOFF.md for me.

Plan defects: where the plan's goals make one resolution natural and its impact stays within the slice or touches only a few slices, decide it yourself and record it as a delegated plan decision. Never edit the plan, widen the surface except by grant, or relax a floor fact. Stop for anything serious, contested or cross-cutting, and whenever the floor or a plan gate needs a human.

Finish: run `status --report`, confirm the ledger was written, run `ledger render`, and read `run-report.md` end to end. Record the run's actual status in HANDOFF.md. Report:
- total run time, quoted from the report with its endpoints
- commits (unpushed)
- what was accepted, and on what evidence
- what stopped, and why
- plan defects, with the decisions you took and the amendments you recommend
- residual risks and follow-ups, with a convergence read
- the leaderboard path
```
