# Mimic Emulator Implementation Plan (Step 6, the calibrated-variance spike)

**Purpose:** Execute the *First Work* of [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md): measure whether a cheap emulator predicts `sage16`'s response well enough and knows honestly when it cannot, and how much of parameter space one history-matching iteration removes. The deliverable is a verdict of pass, fail or error against criteria frozen here, with a report. The code that produces it is written as the first working version of the instrument, so that a pass leaves something a campaign or the model builder can call, and a fail leaves nothing to unpick in Mimic.

**Status:** Planning contract, revision 7 (2026-11-11), revised after two rounds of a four-seat plan panel and confirmed clean by a third. Eight Mode B slices on one feature branch of a repository outside Mimic, of which Slices 1 to 4 are complete; no slice changes a file in Mimic.

- **Revisions 1 to 6 (2026-11-10)** are the plan as run 1 executed it. Revision 1 was reviewed by an independent two-reviewer panel, and revision 2 took its findings; revision 3 made the package's independence from `sage16` a contract (Decision 13) and was re-reviewed by the same panel; revision 4 added Decision 14 and the time-resolution limit after a substep scan; revision 5 required `capture`'s workers to be threads, so that the run works inside a sandbox; revision 6 named the reviewer seats and the run's sandbox policy. Revisions 4 to 6 were not re-reviewed.
- **Run 1** (PM run `20261110T090410Z-ea3675`) completed Slices 1 to 4 under revision 6 and was ended before the diagnostics slice. A read-only analysis of the pilot had found that every bin's count carries noise of Poisson size, which the single constant nugget or white-noise term of the emulators as built cannot represent, so that the spike could have read "emulation does not transfer" from the noise model and not from the method.
- **Revision 7** changes how noise and variance are modelled and reported, and nothing the verdict is judged by. It adds a declared count-noise model (Decision 15) in a new Slice 5; carries it through the diagnostics and the implausibility (Slice 6, revision 6's Slice 5); adds report diagnostics (Slices 7 and 8, revision 6's Slices 6 and 7); records the Bayes linear control finding of run 1 and the owner's reading of it (Decision 16); removes a stale sentence that said the run is not sandboxed; and notes on Slices 3 and 4 how they were executed. Decision 9's criteria and thresholds, the budgets and the gate, the emulated quantity, the constraint, the frozen outputs, the design, the seeds, the measurement protocol, the sandbox and the reviewer seats are unchanged. No evaluation of the main capture existed when revision 7 was written.
- **Review.** Revision 7 was revised after two rounds of a four-seat plan panel (a drift auditor and three reviewers). In neither round was the design contested; the panel verified the noise formulas, the `phi` convention, the uniqueness of the residual variance's fixed point, the fixed-covariance selection score, the controls' margins, every revision-7 figure against its committed file, and that nothing judged was changed after data. This text takes both rounds' findings and the PM's rulings on them. The confirmation round (all four seats, 2026-11-11) returned RESULT: clean; its two optional notes are applied.

**Planning baseline:** Mimic `9ee08e55` on `main` (v1.6), clean tree. Every `path:line` anchor below was read at that baseline, and every measurement under [Frozen Facts and Evidence](#frozen-facts-and-evidence) that revision 6 carried was taken on 2026-11-10 with a `sage16` / `mini-millennium` binary whose physics and core sources equal the baseline's. The commits on Mimic's `main` since then, up to `04ba0591` (the commit run 1 built and measured), touch only `docs/dev/`, so Mimic's measured sources are unchanged since `9ee08e55`. The emulator repository is at `1acd279` on `spike/calibrated-variance`, with a clean tree: run 1 accepted Slice 4 at `56d7115`, and five commits the PM made between the runs add the analyses revision 7 rests on under `evidence/analyses/` and exclude `evidence/` from the formatters and the linter, touching no package, test, spec or pilot file ([Run Preparation](#run-preparation), *Between the runs*). Revision 7's slices start at `1acd279`; `56d7115` stays the reference of the slices' `git diff 56d7115 -- ...` criteria, which those five commits leave valid. PM binds a run to the SHA-256 of this file's bytes.

**Owner:** [Development pathway, step 6](MIMIC-DEVELOPMENT-PATHWAY.md#the-ordered-road). Requirements: [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Future consumers: [`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) (its science gate) and campaigns run under ai-agent-coder's `project-manager` campaign mode. Evidence of record, written by the slices: `reports/SPIKE-REPORT.md` and `evidence/` in the emulator repository.

---

## Decisions for the owner

Each row is a decision this plan takes as frozen unless the owner changes it before the run starts. The recommendation is the plan's text below; an alternative chosen here changes the slices named. Decisions 15 and 16 were taken by the PM of run 1 for revision 7, under the authority the owner granted on 2026-11-10 to revise the noise and variance modelling and the report diagnostics and nothing else.

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
| 13 | **Any model, not only `sage16`.** Mimic is physics-agnostic, and its tools are expected to follow. | **The package depends on Mimic's core contracts only, and everything specific to a model lives in the spec:** the run-file template, the parameters with a scale each (`log` or `linear`, so a parameter may be zero or negative), the property the statistic is taken over, the snapshot, the constraint with its conversions, and the thresholds. Run-file generation leaves every scalar it does not set exactly as the template wrote it. The contract is exercised on `sage16` and, without a run, on `sage26`'s template (a dry run of design and run-file generation). **Its limits are stated:** one kind of statistic (a mass function of one mass-valued property at one snapshot, over every galaxy where that property is positive and with no other selection) and one shape of constraint (a value with a symmetric error), so a model constrained by clustering, occupation or a scaling relation needs one more statistic function; integer switches and pipeline structure stay out of the design vector, as the brief decides (one emulator per structure); a package with nothing to vary has nothing to emulate; and no second model is run here. | Run a second model in this plan: a rebuild of the default binary mid-run, or a second checkout, for evidence the instrument's plan must produce anyway (the brief's gate item 4 needs two real pipelines). Build more kinds of statistic now: code with no caller. |
| 14 | **Time resolution.** `sage16` is not converged at the template's `SubSteps: 10` ([Frozen Facts](#frozen-facts-and-evidence)). | **The spike runs at the template's ten substeps (owner, 2026-11-10).** It tests the method on the shipped model, whose parameters were fitted at that setting and sit inside the acceptable region there; the model is deterministic at any fixed setting, so the test is fair. The limit is stated: the acceptable region the spike finds belongs to the model at ten substeps. | A converged setting (fixed 200 substeps at about 37 s a run, or dynamic 200 at about 16 s): the result would describe the physics and not the time step, but the shipped parameters no longer match the data above the knee. It matters when the tool recalibrates a model, after the coupled-rate work. |
| 15 | **Count noise** (revision 7). A bin's galaxy count carries discreteness noise whose size depends on the count, and the emulators as Slice 4 built them give every training point one constant nugget or white-noise variance. The pilot measured that noise at Poisson size ([Frozen Facts](#frozen-facts-and-evidence)). | **A declared count-noise model, not an estimated one: a variance of `scale (log10 e)^2 / max(N, 1)` in `log10 phi` for a bin holding N galaxies, with `scale` 1 (Poisson noise carried to the logarithm, with the single-galaxy floor), written in the spec before the main capture.** It enters each emulator's fit as a known variance per training point, from that run's own captured count, and is added at a prediction point from the predicted count, in the standardised errors and in the emulator's implausibility. The simulator's own implausibility is unchanged. A synthetic count control at the main capture's sizes must pass with the term and show the miscalibration without it (Slice 5). What the evidence shows and does not show: the roughness is Poisson-sized and uncorrelated between runs at the design's spacing; on a synthetic control at 400 training points the term makes calibration uniform across count bands; on the pilot's own 80-point folds interpolation error dominates and the term shows no gain. Fits without the model, and fits with the term at prediction only, are reported beside the candidates and enter no verdict (Slice 7). | Fit the scale: the likelihood rises with it well past 1 (to the largest value tried, 4), absorbing emulator misfit into the noise, which is tuning the variance to pass. A separate sampling term in the gate's denominator: the emulator's variance already predicts a noisy run, so the term would be counted twice, and at the default run and the gate budget adding it would increase the denominator by at most 2.4% in a core bin. Excluding the low-count bins, or emulating `sqrt(N)` in the verdict: each changes the outputs or the emulated quantity fixed before the pilot was read, so `sqrt(N)` is measured as a transformation (Slice 7) and decides nothing. A residual variance estimated from leave-one-out residuals: on the pilot its ratio to the contracted estimate was 1.00, so it changes nothing there. The Poisson term added at prediction only, on fits with a constant noise: for the Gaussian process it validated the most outputs of any variant on the pilot's 80-point folds, where any added variance helps, but it counts the noise twice; on the synthetic control at the main capture's sizes it overstated the variance (a root-mean-square standardised error of 0.72 for the Gaussian process), and where the true noise was a quarter of Poisson it fell to Decision 9's floor of 0.5 on up to 9 of 16 outputs. It is not a candidate; Slice 7 reports it as a supplementary configuration. |
| 16 | **The Bayes linear residual form** (revision 7). On Slice 4's known-good control the Bayes linear emulator understates its standard deviation by about 15%. It meets the control on the pinned draw the suite asserts and on 69 of 90 further draws; the Gaussian process meets it on 89 of 90 ([Frozen Facts](#frozen-facts-and-evidence)). | **Keep the form as Slice 4 contracts it (selected terms, one isotropic correlation length, residual variance from the generalised residual sum of squares over n minus the number of terms), and carry the shortfall into the report as a finding.** The owner's reading of the Slice 4 control (2026-11-10) stands: its criteria are asserted on the pinned draw, and the rate over further draws is recorded and not gated. The spike asks whether a variance is honest on `sage16`, and the holdout answers that for this form as it does for the Gaussian process; a pass by the Gaussian process where the primary fails is a pass ([Outcome and Limits](#outcome-and-limits)). | A residual variance from leave-one-out residuals: it met the control on 30 of 30 development draws, but on the pilot's fits its ratio to the contracted estimate was 1.00 (0.92 to 1.06 from the 10th to the 90th percentile), so it would change the method's text and not its result on the data in hand. One correlation length per input: 28 of 30, a larger change of method that the Gaussian process already represents; deferred to the instrument's plan. All 84 terms with no selection: 84 of 90, but the inert input becomes active, which the control's own criterion forbids. A shorter correlation-length grid (29 and 30 of 30): tuning the variance by another name, with root-mean-square errors 30% and 73% larger. |

---

## Outcome and Limits

**Delivered when the plan closes:**

- A verdict of pass, fail or error on whether emulation with history matching (a Bayes linear emulator or a Gaussian process) transfers to `sage16`, against criteria fixed before any evaluation of record, with the evidence to check it: the design, every evaluation's outputs, the fitted diagnostics and the report.
- Answers from evidence to the brief's Open Questions 3 (emulator family) and 4 (output transformation), and measurements bearing on Open Questions 1 (budget sensitivity) and 6 (what a campaign's state is).
- A small standalone package, `mimic_emulator`, with three stages (`design`, `capture`, `analyse`), a declared spec that includes the statistic's noise model, tests and controls, usable against any Mimic checkout and, through its spec, any model package constrained by a mass function over every galaxy with a positive value of one property (Decision 13).

**Limits, stated so they are not overclaimed:** one model (`sage16`), one simulation (mini-Millennium), one statistic (the z = 0 stellar mass function), one constraint table in the conventions the shipped figure applies (inherited, not re-derived from the paper), one iteration, one machine, one time resolution (the template's ten substeps, at which the model is not converged), and one declared noise model (Poisson in `log10 phi`, scale 1), which is not fitted. The ranges, the output rule and the removal threshold were set after an exploratory probe of 176 runs, none of them reused. The noise model was set after the pilot of 133 runs and a read-only analysis of it, before any evaluation of the main capture existed; its support at the main capture's sizes is a synthetic control, because the pilot's 80-point folds are dominated by interpolation error. The package's independence from `sage16` is a contract checked by a dry run on `sage26`'s template, not by running a second model. The verdict says whether the method is worth an instrument plan; it does not say `sage16` is well-posed, it validates no gate for mechanical use (the brief's Open Question 7 and the model builder's injected-error requirement stand), and it settles no discrepancy budget: its removal result holds only at the budget it is quoted for. A pass by the Gaussian process where the primary fails is a pass, reported as such, with both configurations judged on the same holdout.

---

## Frozen Facts and Evidence

The numbers this plan designs against, each with its source. A slice that finds one wrong stops rather than silently rebasing. Those revision 6 carried were measured on 2026-11-10 on the Mac Studio, from the Mimic root, in scratch directories, with no tracked file changed; they have no committed output, so the report of Slice 8 states what rests on them in words and quotes none of their numbers. The five blocks revision 7 adds (run 1, the count noise, the Bayes linear control, the count control and the stage controls) name their own sources. Paths under `evidence/analyses/` are in the emulator repository, committed, and readable by every seat, the round-2 probes among them. A copy of some of them is kept under `archive/emulator-spike-analyses/` in the Mimic checkout, which is git-ignored and readable by the PM on the host only: it holds the binary intermediates where an analysis made any, and it is not a full mirror. A fact that rests only on the package's module docstring is labelled so where it is stated.

**The run surface.**

- A `sage16` run on mini-Millennium takes 2.36 to 2.48 s (all eight tree files, eight snapshots, HDF5) and 2.24 s with snapshot 63 alone; one tree file takes 0.30 s whatever is written. Under eight concurrent processes 176 runs took 55.9 s in all (median 2.50 s each). A run with snapshot 63 alone writes about 11 MB.
- The physics path has no random number generator and no threads (`src/` and `models/sage16/` searched). Two runs of one run file give byte-identical binary output and identical HDF5 datasets; HDF5 files differ only in modification-time bytes and `RunProperties/RunEndTime`.
- Concurrent runs with distinct output directories share no written file. Relative paths in a run file resolve against the working directory, so runs are launched from the Mimic root; the run file may live anywhere and the output directory may be absolute.
- The shipped run file `models/sage16/input/sage16_mini-millennium.yaml` has 15 entries under `modules.parameters`, a top-level `SubSteps: 10`, and no `input:` section (the tree range, files 0 to 7, comes from `simulations/mini-millennium/simulation_info.yaml:4-5`). A run file re-emitted by PyYAML (`safe_load`, then `safe_dump(sort_keys=False)`) at the template's values gives snapshot-63 galaxies byte-identical to the template's in all 42 fields, and 36,530 rows. Read back with PyYAML's `BaseLoader`, which keeps every scalar as its written text, that round trip changes one scalar of the template's 50 (the empty `post_timestep:` becomes `null`), and none of the 70 in `models/sage26/input/sage26_mini-millennium.yaml`. Mimic's reader uses libyaml's document API and never inspects a scalar's quoting style (`src/core/read_parameter_file.c`), so a quoted scalar reads as the plain one.
- Mimic does not reject a misspelled top-level key (a misspelled `SubSteps` silently runs with one substep) or an extra name under `modules.parameters`; a required parameter that is missing, an out-of-range value and a non-numeric value each exit 1 before any tree is read. So a generated run file is trusted only after the run: the master file's `RunProperties/Parameters` dataset records every name and the value text exactly as written (2,640 entries checked against `repr`), and `RunProperties` carries `ModelName`, `SimulationName`, `FirstFile`, `LastFile`, `SubSteps`, `BoxSize` and `Hubble_h` as attributes.
- A successful run exits 0 and writes `metadata/version_info.json` last; a failed one leaves an empty output directory.
- The model is not converged in time resolution at the template's `SubSteps: 10`. A scan at the shipped parameters to 1000 substeps under both timestep schemes (20 runs) shows the two schemes converging to one answer, roughly first order in the step. Against 1000 fixed substeps, the default run's total z = 0 stellar mass is 66% higher, and its mass function differs by 0.08 dex RMS (0.16 dex at most) over the probe's 16 core bins and by 0.4 to 0.8 dex from 10.85 to 11.25; at 200 fixed substeps those figures are 1.9% and 0.005 dex RMS. The shipped parameters match the data at ten substeps (0.04 dex RMS over the core bins) and not when converged (0.10 dex RMS, and 0.3 to 0.7 dex low from 10.75 to 11.25).
- The other shipped packages differ where a spec must carry the difference. `sage26` has 28 parameters: 10 are integers, `FracZleaveDisk` is 0.0, and `MShockMsun: 6.0e11` is text to PyYAML. `sham` and `hod` have negative parameters; `sham` runs only under the horizontal driver, and `hod` ships vertical and horizontal run files and has no `StellarMass`. A horizontal run's master file (`hod.hdf5`, from `hod` on micro-Uchuu) carries the same `RunProperties` and is read by the same `read_hdf5_snapshot`.
- Accepted ranges, from the modules' `init()` (the stricter of two where two modules load one parameter): `SfrEfficiency` [0, 1]; `FeedbackReheatingEpsilon` (0, 10]; `FeedbackEjectionEfficiency` [0, 10]; `ReIncorporationFactor` [0, 10]; `RadioModeEfficiency` [0, 1]. All 32 corners of Decision 5's box ran and exited 0.

**The statistic and the constraint.**

- The shipped figure computes the model's mass as `log10(StellarMass * 1e10 / h)` over `StellarMass > 0`, all galaxy types (`models/sage16/plots/figures/stellar_mass_function.py:98`), and its mass function through `calculate_mass_function(mass, volume, h, 0.1, 8.0, 13.0)` (`plot/mimic-plot/output_utils.py:293`: counts divided by the volume in (Mpc/h)^3 and the bin width, times h^3).
- The Baldry table is a 50-row literal at lines 149 to 203 of that figure (log10 M from 7.05 to 11.95 in steps of 0.1, phi, sigma_phi), converted at lines 206 to 211: mass `log10(10^x / h^2) - 0.26`, phi and its error times h^3. The last three rows have `sigma_phi` equal to `phi`. The same table is copied in `models/sage26` and `models/sham`; Mimic has no shared observations module.
- `plot/mimic-plot/hdf5_reader.py:48` (`read_hdf5_snapshot`) and `plot/mimic-plot/output_schema.py:99` (`mass_to_msun`, which raises on a unit label that is not a mass) are importable by inserting `plot/mimic-plot` in `sys.path`; the loader that knows volume and h lives in the script `mimic-plot.py`, which is not importable by name, so the package reads `BoxSize` and `Hubble_h` from `RunProperties`.

**The response, from a probe of 176 runs over Decision 5's ranges** (the two controls, 32 corners, 60 Latin-hypercube points, and two 41-point scans; the probe is not evidence of record and its runs are not reused).

- The model's counts peak at a bin centre of 8.46 and fall below the data under about 8.5 (resolution); at centres of 9.0 and above the default model is within 0.08 dex of the data in every bin that holds more than 3 galaxies.
- At the default the bins hold 920 galaxies at 9.06, 392 at 10.46, 176 at 10.96, 40 at 11.26 and 3 at 11.46. Across the ranges the massive end empties: 17 of 60 Latin-hypercube runs have no galaxy at 11.26, and 5 of 32 corners have none at 10.76.
- Decision 7's rule keeps 16 bins on the probe (9.05 to 10.55 in the table's masses); with corners and every run required it keeps 13.
- The response is smooth but not simple: `SfrEfficiency` gives a non-monotonic count at 9.96 and plateaus above 0.1, and `RadioModeEfficiency` changes the bins at 9.96 and below by less than 0.05 dex over its whole range, acting mainly above 10.5.
- Withdrawn in revision 7: revision 6 recorded here a run-to-run roughness of 2 to 5 galaxies per bin (0.002 to 0.005 dex in the core bins), five to ten times below the Poisson scale. The pilot does not bear that out between runs at the design's spacing, where the roughness is of Poisson size (*The count noise*, below). Why the probe's figure differs was not investigated, and nothing in revision 7 rests on it.
- Classified directly, with no emulator, by the rejection rule on those 16 bins, 0.22, 0.37, 0.50, 0.63, 0.75 and 0.85 of the 60 Latin-hypercube runs are non-implausible at the six budgets.

**Run 1 and the commits between the runs** (PM run `20261110T090410Z-ea3675` in the emulator repository, 2026-11-10; sources: that repository's commits, `reports/PILOT-RECORD.md` and `evidence/pilot/`).

- Slices 1 to 4 were accepted at `47d7671`, `d8e8798`, `0947d42` and `56d7115`.
- Between the runs the PM made five commits on the branch, none of which touches the package, its tests, the spec, the constraint, the pilot evidence or the pilot record (`git diff --stat 56d7115..1acd279` names only `evidence/analyses/` and `pyproject.toml`): `2b546eb` adds the first three analyses below under `evidence/analyses/` (scripts and text outputs, with a `README.md`); `6d40d79` excludes `evidence/` from black, isort and ruff in `pyproject.toml`, because the one-off scripts are evidence and not package code; `bf01d12` adds the outputs of rerunning the Bayes linear review's scripts; `0071e65` adds a reproduction of the control's draw counts over its three sets and the round-2 probes; `1acd279` corrects the analyses' `README.md`. The branch head is `1acd279`, and the package code there equals that at `56d7115`.
- The pilot ran once: 133 of 133 runs verified in 44.9 s at eight workers, 1.5 GB, the runs reporting Mimic `v1.6-7-g04ba0591`, `BoxSize` 62.5 and `Hubble_h` 0.73; the default row has 36,530 rows at snapshot 63. Decision 7's rule froze 16 core outputs, 9.05 to 10.55, and 8 exploratory ones, 10.65 to 11.35.
- Counts in the frozen outputs, over the pilot's 100 Latin-hypercube runs (`evidence/pilot/outputs.csv`, summarised in `s01.out` of the analysis below): of the 1,600 run and core-output pairs, none is zero, 27 hold fewer than 30 galaxies, 121 fewer than 100 and 1,228 at least 300; the two highest core bins fall to 2 and 4 galaxies. Of the 800 run and exploratory-output pairs, 84 are zero and 334 hold fewer than 30.
- The SHA-256 of `./mimic` and of the template and simulation files in the Mimic checkout on 2026-11-11 equal those in `evidence/pilot/context.json`.

**The count noise, from a read-only analysis of the pilot** (2026-11-10; scripts and outputs in `evidence/analyses/2026-11-10-pilot-noise-model/`, files `s01.out` and `s03.out` to `s07.out`; `s01.out` and `s03.out` are from rerunning their scripts unchanged on 2026-11-11). The analysis read `evidence/pilot/` and the package at `56d7115` and changed neither. No holdout was involved: the main capture does not exist. Its fits "with the term" are prototypes, not Slice 5's code. The Gaussian process's has Slice 5's form (`GPhet(c=1)`: the declared variance per training point and at the predicted count). The Bayes linear emulator's (`BLknown(c=1)`) has Slice 5's covariance, but takes five updates of the residual variance with no convergence check and chooses the length and nugget by a score that rescales the declared noise; it supports the recorded measurements and does not validate Slice 5's estimator.

- **Size.** Within a run, the mean square of a bin's `log10` count about the cubic through its four neighbouring bins is 0.98, 1.03, 1.00, 1.02 and 1.17 times the Poisson variance (0.99 to 1.08 as ratios of standard deviation) in the count bands 5 to 30, 30 to 100, 100 to 300, 300 to 1,000 and above 1,000 (core and exploratory bins of all 133 runs; `s01.out`). A moment estimate of the scale from the Gaussian process's leave-one-out residuals on the 100 Latin-hypercube runs is 0.89, and 0.91 with the term in the fit (`s05.out`).
- **Independence.** The correlation of that Poisson-standardised scatter between Latin-hypercube runs is at most +0.08 in any band of distance between the runs (`s05.out`) and +0.09 between nearest neighbours (`s03.out`), against a median nearest-neighbour distance of 0.22 for 400 Latin-hypercube points in five inputs. At the design's spacing the noise is close to white, so an emulator cannot resolve it. It is not white along a direction a bin does not respond to: corner runs that differ only in `RadioModeEfficiency` share their scatter (+0.50; `s03.out`).
- **What the pilot cannot show.** At 80 training points (five folds of the 100 Latin-hypercube runs) the cross-validated error of the Gaussian process is 2.4 to 4.8 times the Poisson level across the four count bands, and that of the Bayes linear emulator 2.4 to 6.5 times (`s03.out`): interpolation error dominates. Under Decision 9's per-output rule the outputs validated, of 16, averaged over ten fold assignments, are 11.6 for the Gaussian process as built and 11.0 with the term in its fit and at prediction (`s04.out`); for the Bayes linear emulator they are 5.4 as built (`s04.out`) and 5.4 with the declared term in the prototype (`s07.out`, line b). A different variant, a count-dependent nugget whose size is chosen by leave-one-out (`BLhet(tau LOO)`, which estimates what Decision 15 declares), also gives 5.4 (`s04.out`). The Gaussian process's root-mean-square standardised error below 30 galaxies falls from 2.44 to 1.80 (`s04.out`). So the pilot neither supports nor refutes the term's effect at 400 points.
- **At the main capture's sizes, on a synthetic control** (the pilot's fitted response as the truth, Poisson counts, 400 training and 200 test points, 16 outputs, three replicates; `s06.out`). Root-mean-square standardised errors are given for the count bands below 30, 30 to 100, 100 to 300, and 300 and above. The Gaussian process as built validates 14, 16 and 15 of 16 outputs, with 2.32, 1.58, 1.19 and 0.88; with the term in the fit and at prediction, 16, 16 and 16, with 1.01, 1.05, 0.98 and 0.97. The Bayes linear emulator as built validates 14, 16 and 14, with 2.16, 1.51, 1.17 and 1.01; the prototype with the declared term, 13, 14 and 15, with 1.03, 1.17, 1.11 and 1.23: uniform across the bands, and over-confident by about the amount of Decision 16.
- **If the declared scale is too large** (the same control with a quarter of the Poisson variance): the Gaussian process with the term has a root-mean-square standardised error of 0.55 and validates 15, 15 and 16 outputs (`s06.out`). An overstated scale shows as a value near Decision 9's floor of 0.5.
- **The alternatives of Decision 15.** Fitting the scale: the Gaussian process's log marginal likelihood, summed over the 16 outputs, gains 32.7 at scale 1, 42.8 at 2 and 46.9 at 4, the largest tried (`s05.out`). A sampling term in the gate: at the default run and the gate budget, adding the Poisson variance would increase the gate's denominator by at most 2.41% in a core bin (`s01.out`); in a low-count run the increase is larger. A leave-one-out residual variance: its ratio to the contracted estimate over the 80 pilot fits has median 1.00, and 0.92 to 1.06 from the 10th to the 90th percentile (`s07.out`). The term at prediction only, on fits as built: on the pilot's folds it validates the most outputs of any Gaussian-process variant tried (14.0 of 16), because at 80 points it widens a variance that interpolation error has left too small; for the Bayes linear emulator the same fixed-scale term gives 6.5, and a variant that fits the scale of the added term reached 6.6 (`BL+cPois(add)`; all in `s04.out`); on the synthetic control at 400 points it overstates the variance (the Gaussian process's root-mean-square standardised error is 0.72 overall and 0.68 at 300 galaxies and above), and with a quarter of the Poisson variance it validates only 13, 7 and 9 outputs (`s06.out`). Slice 7 reports this configuration beside the others.
- The PM's record of the analysis also carries an extrapolation, that at 400 training points count noise is about 40% of the error variance. No saved output states it, and nothing here depends on it.

**The Bayes linear control, from run 1** (sources: `evidence/analyses/2026-11-11-control-draw-sets/control_draw_sets.out` for the draw counts of the three sets and the failing draws, made by the PM on 2026-11-11 with the tests' own draw and metric helpers; `evidence/analyses/2026-11-10-bl-calibration-review/`, files `e0_baseline.out` to `e6_hetero.out`, for the rest, from the PM's rerun of the review's scripts on 2026-11-11. Both reproduce the figures recorded during run 1, which also stand in the module docstrings of `mimic_emulator/emulator.py` and `tests/test_emulator.py`).

- Draws of Slice 4's known-good control meeting every criterion, of 30 in each of the sets 1000 to 1029, 2000 to 2029 and 3000 to 3029: the Bayes linear emulator with its selected terms 23, 25 and 21 (69 of 90); with all 84 terms and no selection 26, 29 and 29 (84 of 90); the Gaussian process 30, 29 and 30 (89 of 90). The suite asserts the criteria on one pinned draw. With all 84 terms the inert input is active on all 30 draws of every set; with the selected terms it is active on 0, 1 and 1 draws.
- Every Bayes linear failure is over-confident: 0.800 to 0.895 of standardised errors within 2, against the bound of 0.90, and a root-mean-square standardised error of 1.20 to 1.49 (`control_draw_sets.out`). The fits are accurate: on draws 1000 to 1029 the root-mean-square error is 0.009 to 0.018 of the function's standard deviation with the selected terms (`e0_baseline.out`). A bound of 0.029 over every failing draw of either term set is recorded in the module docstring only; no saved output; Slice 8 does not quote it.
- The over-confidence is a bias of about 15% in standard deviation across the draws and not a few bad draws: over draws 1000 to 1029 the mean root-mean-square standardised error is 1.14 on the test points and 1.15 by training leave-one-out (`e1_variants.out`, row `base`).
- The criteria seldom fail a calibrated emulator on designs of this size. On 120 Gaussian-process truths drawn at the designs of draws 1000 to 1029 (60 with the anisotropic kernel the package's Gaussian process fitted to control draw 1000, and 60 with every active length set to their geometric mean), an oracle that predicts with the true kernel and noise met the calibration criteria on 60 of 60 and 60 of 60 (`e3_sim.out`). That experiment is not Slice 4's control function with an exact variance, so it bounds the criteria's chance failure rate and does not by itself show the control to be fair to the Bayes linear form. On the same truths the Bayes linear emulator as built met them on 60 of 60 isotropic and 56 of 60 anisotropic ones, which points at the single correlation length.
- Variants on draws 1000 to 1029, of 30 (`e1_variants.out`, `e4_more.out`): as built 23; residual variance from leave-one-out residuals 30; one correlation length per input 28; all 84 terms 26; the length grid cut off at 0.5 and at 0.3, 29 and 30, with root-mean-square errors 30% and 73% larger. Scoring candidate terms by universal-kriging leave-one-out error met the control on 45 of 60 development draws against 48 for the selection as built, and was about fifty times slower: recorded in the module docstring only; no saved output; Slice 8 does not quote it.
- On the pilot's fixed five folds the outputs validated, of 16, are 7 for the Bayes linear emulator as built, 8 with the leave-one-out residual variance and 8 for the Gaussian process (`e5_pilot.out`); by count band the root-mean-square standardised errors are 3.65, 1.86, 1.81 and 1.31 for the Bayes linear emulator and 3.07, 1.76, 1.46 and 1.28 for the Gaussian process (`e6_hetero.out`).

**The count control of Slice 5, from a planning probe for this revision** (2026-11-11; the package imported read-only; script and outputs in `evidence/analyses/2026-11-11-count-control-probe/`, main output `chosen-map-draws-7000-7029.out`). The control is the one Slice 5 specifies: Slice 4's control function mapped to an expected count of about 5 to 2,000, Poisson draws, 400 training and 200 test points a draw. The fits with the term were the probe's own code and the fits without it are the package's. The probe's Bayes linear fits with the term found the residual variance by the plain iteration and chose the length and nugget by the score that rescales the declared noise; `root_search_check.py` and a round-2 check (last bullet) show that Slice 5's root search and fixed-covariance score give the same figures.

- **Thirty draws (seeds 7000 to 7029), pooled.** With the term, the Gaussian process has 0.939 of standardised errors within 2, 0.005 beyond 3, a root-mean-square standardised error of 1.06, and 1.12, 1.10, 1.07 and 1.03 in the four count bands; the Bayes linear emulator 0.941, 0.005, 1.05, and 1.10, 1.08, 1.05 and 1.02. Without the term, the Gaussian process has 2.27, 1.30, 0.82 and 0.62 in the bands and the Bayes linear emulator 2.25, 1.30, 0.85 and 0.63: over-confident where counts are low and under-confident where they are high. For the fits with the term the root-mean-square error against the noise-free function is at most 0.081 of its standard deviation on any draw; without it the Bayes linear emulator reaches 0.099.
- **In the six groups of five draws.** With the term every band value lies in 0.97 to 1.18 (Gaussian process) and 0.98 to 1.14 (Bayes linear), and the pooled fraction within 2 in 0.929 to 0.950. Without it the Gaussian process's value below 30 galaxies is 2.12 to 2.50. On the first group, seeds 7000 to 7004, which Slice 5 asserts: Gaussian process 0.938 within 2, 0.008 beyond 3, 1.06 overall, bands 1.06, 1.11, 1.11 and 1.03; Bayes linear 0.943, 0.007, 1.04, bands 1.05, 1.09, 1.07 and 1.01; Gaussian process without the term 2.12 below 30.
- **Why the control pools draws.** One 200-point test set puts 13 to 32 points below 30 galaxies and 26 to 47 in each of the two middle bands, and a band's value then scatters by sampling alone: all four bands lie in 0.7 to 1.3 on 21 of the 30 single draws for the Gaussian process and 24 for the Bayes linear emulator, though both are calibrated when pooled. On single draws the fraction within 2 falls to exactly 0.900. The band figures of the pilot analysis pool 3,200 points a replicate.
- **Why the control's counts are mostly high.** With the same function mapped linearly to the logarithm of the count, a fifth of the points lie below 30 galaxies, the constant noise term fits to them, and the Gaussian process without the term has only 1.51 below 30 (eight draws pooled; 1.02 to 1.94 singly; `explored/linear-map-draws-6000-6007.out`). The failure the term corrects needs an output whose counts are mostly high and sometimes low, as the frozen core outputs are.
- **The residual variance's fixed point.** The plain iteration (the variance replaced by the generalised residual sum of squares over n minus p, to a relative change of 1e-8) took up to 91 updates on a grid point of the control and, where noise dominates (the linear mapping), had not converged after 500 updates on up to 9 of the 90 grid points of a fit. A bracketed root search on the logarithm of the variance (Brent's method) took at most 17 evaluations a grid point, reached the same fixed point (to 1.5e-7 relative, the plain iteration's own error) and the same chosen grid point on ten draws, and met the fixed-point equation to 1e-11 (`root-search-logistic_0.7_0.5.out`, `root-search-linear.out`; the 91 updates are in `chosen-map-draws-7000-7029.out`).
- **Other readings.** With the term the inert input is excluded by the Bayes linear emulator and has the Gaussian process's longest length scale on all 30 draws; no fit reached the residual-variance floor; no count was zero. A Bayes linear fit with the term took 3 to 6 s by the plain iteration and about 1.4 s by the root search, and the two Gaussian-process fits of a draw 6 to 15 s together, one thread each, under a 28-process load.
- **Round 2: the selection score Slice 5 contracts** (2026-11-11; `evidence/analyses/2026-11-11-count-control-probe/round2/`, files `noise_fits.py`, `fixed_covariance_score_check.py` and `fixed-covariance-score-draws-7000-7029.out`). With the residual variance from the root search and the length and nugget chosen by the leave-one-out density under the fixed full-fit covariance, the Bayes linear emulator's pooled figures on every group of five draws and on all 30 equal those above to the digits quoted. The fixed-covariance score and the rescaling one choose the same length and nugget on 28 of the 30 draws, all of 7000 to 7004 among them, and differ by one step of the nugget grid on the other two.

**The stage controls of Slice 6, from a round-2 probe** (2026-11-11; `stage_control_probe.py` and `stage-control-seeds-8000-8009.out` in the same `round2/` directory; fits written to Slice 5's contract, diagnostics to Slice 6's). The controls are the ones Slice 6 pins: six core outputs whose expected counts are powers of ten of Slice 4's control function at the shipped design's 400 `train` and 200 `holdout` rows, Poisson counts from seed 8000, and a constraint at the counts' central values with an error of 0.05 dex.

- **Known-good, seed 8000.** Both emulators validate 6 of 6 outputs (fractions within 2 of 0.940 to 0.970; root-mean-square standardised errors 0.92 to 1.10). At the gate budget of 0.1 dex the simulator keeps 82 of the 200 holdout runs, against the minimum of 20; neither emulator rejects any of them, against a limit of 0.05; and the non-implausible fraction of the box is 0.485, against a limit of 0.75. The emulators reject 0.98 to 0.99 of the runs the simulator rejects. The first candidate passes, so the verdict is `pass` by the primary.
- **Known-good, seeds 8000 to 8009.** Both configurations pass on all ten. The simulator keeps 80 to 87 runs at the gate budget, the false exclusions are 0 to 2 of them (at most 2.4%), and the non-implausible fraction is 0.483 to 0.533. The fewest outputs validated is 5 of 6 (Bayes linear, one seed, which gives the 87 and the 0.533), and that still meets the 0.75 the spec asks.
- **Known-bad.** With the holdout rows' expected counts multiplied by `10^(0.3 sin(2 pi u_1))`, no output validates for either emulator on any of the ten seeds (fractions within 2 of 0.04 to 0.13), so neither configuration is calibrated.
- The counts run from 44 to about 63,000 and none is zero: these controls exercise the verdict logic, and the count control of Slice 5 exercises the noise term.
- **Profiles, for Slice 7's criterion on this control** (a drafting check with no saved output: the rejection rule applied to the noise-free expected counts at 100,000 Latin-hypercube points, no emulator fitted). At the gate budget the non-implausible fraction is 0.48; in 20 slices of each input the inert control's profile stays within 0.013 of its mean, against the 0.05 the criterion allows, and the fourth input's spans 0.295, against the 0.2 it asks of at least one active input. The other active inputs span 0.07, 0.16, 0.14 and 0.17, so only one clears 0.2 and that margin is thin; Slice 7 stops for the PM if the criterion fails.

**The method, from the sources.** PRISM fits, per output, a polynomial of order 3 in the active parameters chosen by cross-validated stepwise selection, takes the in-sample mean squared error as the residual variance, uses a Gaussian correlation with a fixed length of 0.3 of each range, and inverts with a pseudo-inverse; it has no held-out diagnostic, and added a variance inflation of 0.2 because its variance was found to be underestimated (PRISM source, `prism/emulator/_emulator.py`, v1.3.2; van der Velden et al. 2019, sections 2.3 and 3.3). The Meraxes analysis used 500 evaluations for 9 parameters in its first iteration and reported no held-out test. The validation literature gives the diagnostics used here: individual standardised errors with values beyond 2 marking conflict (Bastos & O'Hagan 2009, section 3.2), and the comparison of emulator with simulator implausibility at validation points, with 0.05 as the yardstick for a wrongly excluded point (Iskauskas et al. 2022, the `hmer` package, section 3.2). Published discrepancy budgets: Bower et al. (2010, section 3.6) took 0.075 dex for the structural term of a luminosity function, beside separate terms for the finite number of sub-volumes (0.014 to 0.022 dex) and for inactive parameters; Rodrigues, Vernon & Bower (2017) adopted 0.1 dex in all for the stellar mass function. Iskauskas et al. add that a wrongly excluded output matters only where the other outputs would keep the point, which is why the false-exclusion rate here is counted over runs. An output that fails validation is left out of that iteration's implausibility (the practice reported for Andrianakis et al. 2015, read here through a summary and not in full).

**Environment.** Python 3.14.6; the pins numpy 2.3.4, scipy 1.18.1, h5py 3.15.1, PyYAML 6.0.3 and matplotlib 3.10.7 are Mimic's own (`requirements.txt`); scikit-learn 1.9.1, pytest 9.2.0 and ruff 0.17.4 resolve against them for this interpreter (a dry-run install, nothing installed). Machine: Mac Studio, 32 cores.

---

## Design

**One spec, three stages.** A campaign spec (YAML) declares what is varied, what is measured, what it is compared with and what counts as a pass. `design` turns the spec into a table of evaluations. `capture` runs them against a Mimic checkout and records, per run, the galaxy count in every candidate bin; it is the only stage that needs Mimic, and it runs once. `analyse` reads a capture's small files and the spec, fits the emulators on the training set, tests them on the holdout, computes implausibility against the constraint and writes the verdict. Every number the report states is in a file a stage wrote or in the pilot's record.

**The evaluation is checked after it runs, not before.** Mimic accepts a run file with a misspelled key, so each run is verified from what it recorded: exit status, the completion marker, and the `RunProperties` attributes and parameter table against the design row.

**The emulated quantity** is `log10 phi` per core output, with a zero count set to one before the logarithm (the single-galaxy floor) and the number of floored values reported. The constraint enters as `log10 phi` with standard deviation `sigma_phi / (phi ln 10)`.

**Count noise** (Decision 15). A bin holding N galaxies has `phi = N h^3 / (BoxSize^3 * w)`, with w the bin width in dex, so `log10 phi` is `log10 N` plus a constant, and the count's discreteness gives it a noise variance the spec declares: `v(N) = scale (log10 e)^2 / max(N, 1)`, in dex squared. An emulator is fitted with `v(N)` of each training run's captured count as a known variance at that point, beside its own nugget or white-noise term. Its `predict` then returns the emulator's variance `Var_k(x)`, with the declared noise left out. A new run at x carries the noise of its own count, which is not known in advance, so it is taken at the predicted count: `N_pred = 10^E * BoxSize^3 * w / h^3` for a predicted `log10 phi` of E. The variance of a new run about the prediction is `Var_k(x) + v(N_pred)`, and a standardised error on the holdout is the run's value minus E over the square root of that.

**Implausibility** for output k at point x is `|E_k(x) - z_k| / sqrt(Var_k(x) + v(N_pred) + sigma_k^2 + b^2)`, with E and Var the emulator's mean and variance, `v(N_pred)` the count noise at the predicted count, `z_k` and `sigma_k` the constraint and b the discrepancy budget, all in dex. The simulator's own implausibility at a run replaces E with the run's value and drops both Var and the count noise: it is `|y_k - z_k| / sqrt(sigma_k^2 + b^2)`, as in revision 6. A point is rejected when the second-largest implausibility over the validated core outputs exceeds 3.

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
| Another model, simulation or statistic later | Model, simulation, template, parameters with their scales, the statistic's property and snapshot, its noise model, outputs, constraint and thresholds live in the spec, and code names none of them; another kind of statistic is one more named function |

Not met here, and owed by the instrument's plan: a second model run, further kinds of statistic as models need them, mutation evidence for the gate, a constraint set shared with the figures, refocusing, comparison between structures, and a decision on where the package lives.

---

## Run Preparation

Done once by the owner, or by the PM session at the owner's go-ahead, before `init`. Nothing here is slice work: the dependency manifest and the environment are kept out of every slice's surface.

**Status: completed on 2026-11-10, before run 1, and not repeated for run 2.** The steps below are kept as the record of how the binary, the repository and its environment were made. In particular `./mimic` is not rebuilt: the pilot's runs reported the version the binary was built at (`v1.6-7-g04ba0591`), a rebuild after any later commit in Mimic would report another, and a version or SHA-256 that differs between the pilot and the main capture stops the run. After run 1 the PM made five commits that are not slice work (*Between the runs*, below). Before run 2's `init` the PM checks only what is listed under [Execution Structure](#execution-structure), *Before run 2*.

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

**Between the runs (2026-11-11).** After run 1 ended at `56d7115`, the PM made five commits on `spike/calibrated-variance`, under the owner's grant and outside any slice. None touches the package, its tests, the spec, the constraint, the pilot evidence or the pilot record.

- `2b546eb` adds the three analyses that informed revision 7 under `evidence/analyses/` (scripts and text outputs, no binary intermediates, with `evidence/analyses/README.md`), so that the report can cite files.
- `6d40d79` excludes `evidence/` from black, isort and ruff: the analysis scripts are one-off evidence, they failed the repository-wide gates every slice runs, and reformatting them would alter evidence after the fact.
- `bf01d12` adds the outputs of rerunning the Bayes linear review's scripts (`e0` to `e6`), which had not been saved; the reruns reproduce the figures recorded during run 1.
- `0071e65` adds `evidence/analyses/2026-11-11-control-draw-sets/` (the known-good control on draws 1000 to 1029, 2000 to 2029 and 3000 to 3029 for the Bayes linear emulator with selected terms and with all 84 terms and for the Gaussian process, with the failing draws) and `evidence/analyses/2026-11-11-count-control-probe/round2/` (the two round-2 probes, scripts and text outputs).
- `1acd279` corrects the count and the authorship in `evidence/analyses/README.md`.

The branch head, and run 2's base, is `1acd279`. `pyproject.toml` there, which is what every check of run 2 reads (the three exclusion lines are the only change since run 1):

```toml
[tool.black]
line-length = 100
extend-exclude = "^/evidence/"

[tool.isort]
profile = "black"
line_length = 100
extend_skip_glob = ["evidence/*"]

[tool.ruff]
line-length = 100
target-version = "py312"
extend-exclude = ["evidence"]

[tool.ruff.lint]
select = ["F", "E9", "B", "T10", "DTZ", "PLE"]
ignore = ["F541"]

[tool.pytest.ini_options]
testpaths = ["tests"]
```

**3. The sandbox.** The run's base policy is `archive/sandbox-policies/MIMIC-EMULATOR-SANDBOX.json` in the Mimic checkout, passed to `init` as `--sandbox` with its absolute path. It is kept in the gitignored `archive/` and not committed, because it lists this machine's private paths; as checked below its SHA-256 is `5d9c46437f258e0e44561edaa786f227f2a6763d74eba7faecd21340c9c21153`. `init` validates it and records that digest, so it must not change during the run. Only the PM, on the host, reads it: the policy itself denies reads of `archive/` from inside. It allows the three seat harnesses their hosts and their own state (Claude Code, Codex, and OpenCode with `opencode.ai:443` and the two key files its configuration references), leaves the Mimic checkout, `docs/dev/` and the mini-Millennium trees readable, and denies reads of credentials, private notes, other sessions' transcripts, campaign dossiers and results, and the `archive` and `.orchestrator` directories of Mimic and the toolkit. It grants no write in Mimic: the toolkit adds the run's repository, its git directory and a temporary directory, which is everything a slice writes. `~/.matplotlib` is writable under the policy; the figure slice does not rely on it, because it points `MPLCONFIGDIR` at a temporary directory.

Checked on 2026-11-10 under `srt` 0.0.81, with a scratch directory standing in for the repository grant: the toolkit's base-policy checks pass with no warning; each denied read and write fails and each needed read succeeds; `./mimic -q` run from the Mimic root completes and writes its master file; a `ThreadPoolExecutor` runs two evaluations as subprocesses; a `ProcessPoolExecutor` fails with `PermissionError`, which is why Slice 2 requires threads. Not checked: a real harness inside the sandbox. Before the first slice, make one real-model launch per harness, including one `review` per reviewer harness, as the toolkit's `references/sandbox.md` asks (*Producing your policy*, steps 7 and 8).

---

## Execution Structure

- **Repository and branch.** The run's repository is `/Users/dcroton/Local/git-repos/mimic-emulator`; the branch is `spike/calibrated-variance`, which run 1 created. Its head is `1acd279`: Slice 4's accepted commit `56d7115`, then the five commits made between the runs. This plan stays in Mimic and is passed to `init` by its absolute path.
- **Two runs.** Run 1 (`20261110T090410Z-ea3675`) executed Slices 1 to 4 under revision 6 and was ended. Run 2 executes Slices 5 to 8 under this revision, on the same branch: its `init` takes `--branch spike/calibrated-variance` and `--attest "Slice 1,Slice 2,Slice 3,Slice 4"`. The contract text of Slices 1 to 4 is revision 6's, unchanged, with a status note under each heading; slice numbers inside that text are revision 6's, whose Slices 5, 6 and 7 are now Slices 6, 7 and 8.
- **Before run 2.** The PM checks, and changes nothing: the emulator repository is on `spike/calibrated-variance` at `1acd279` with a clean worktree, and `git diff --stat 56d7115..1acd279` names only `evidence/analyses/` and `pyproject.toml`; `.venv/bin/python -m pytest -q -rs` passes there with `MIMIC_ROOT` set; the Mimic status line below prints nothing; the SHA-256 of `./mimic` and of each Mimic file listed in `evidence/pilot/context.json` equals the value recorded there; and the sandbox policy's SHA-256 is the one [Run Preparation](#run-preparation) records.
- **Mimic is read and executed, never written.** No slice writes in the Mimic checkout, and every slice ends with `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` printing nothing: those three paths are what the run reads besides the binary. Other work may go on in that checkout during the run, provided nobody rebuilds `./mimic` or changes those paths between Run Preparation and the end of Slice 8; each capture records the SHA-256 of the binary and of every Mimic file it reads, and a difference between the pilot's and the main capture's stops the run.
- **Checks PM runs per slice.** `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` (no skip is acceptable with `MIMIC_ROOT` set), `.venv/bin/black --check .`, `.venv/bin/isort --check-only .`, `.venv/bin/ruff check .`, `grep -rn -i -E 'sage16|SfrEfficiency|baldry|StellarMass' mimic_emulator/` printing nothing, and the Mimic status line above. Python style follows Mimic's STYLE-GUIDE (*Python Style Beyond Formatting* and *Python Docstrings*).
- **Measurement protocol.** Evaluations run from the Mimic root with `./mimic -q`, eight at a time, each into its own new directory, each with a 120 s timeout. A capture is made once; a correction that would re-measure stops the slice for the PM. Captures go under the repository's git-ignored `output/`.
- **Stops.** A capture that exits 2, a Mimic version or recorded Mimic SHA-256 that differs between the pilot and the main capture, fewer than 8 core outputs from the pilot, or a frozen fact found wrong, each stop the slice with a question to the PM. A verdict of fail, faithfully reported, is acceptable work.
- **Developer seats** (`Recommended Developer:` per slice; full model ids for the PM):

| Slice | Recommended Developer | Effort | Difficulty | Risk | Reason |
|---|---|---|---|---|---|
| 1 (complete) | `claude-sonnet-5-5` | high | moderate | standard | Tightly specified validation and a deterministic design |
| 2 (complete) | `claude-opus-5-5` | medium | moderate | standard | Subprocesses, HDF5 verification and units: a silently wrong run is the hazard |
| 3 (complete) | `claude-sonnet-5-5` | high | easy | standard | Runs fixed commands and applies a fixed rule |
| 4 (complete) | `claude-opus-5-5` | high | hard | elevated | Numerics where a wrong variance is the failure under test |
| 5 | `claude-opus-5-5` | high | hard | elevated | Numerics again, in the fitted variance, and a declared model added to the frozen spec |
| 6 | `claude-opus-5-5` | high | hard | elevated | The verdict logic and its controls |
| 7 | `claude-sonnet-5-5` | high | moderate | standard | Reporting analyses and figures over finished numerics |
| 8 | `claude-opus-5-5` | high | moderate | elevated | The measurement of record and its report |

- **Reviewers.** Drift auditor `opencode-go/deepseek-v4.1-flash` on OpenCode at max effort; code reviewers `claude-opus-5-5` at high effort, `gpt-6.1-sol` on Codex at high effort (pass `high` explicitly to Codex) and `opencode-go/qwen3.8-flash` on OpenCode at xhigh effort.
- **Sandbox.** The run is sandboxed: `init` takes `--sandbox /Users/dcroton/Local/git-repos/mimic/archive/sandbox-policies/MIMIC-EMULATOR-SANDBOX.json` ([Run Preparation](#run-preparation), step 3). The policy is written for these seats' three harnesses (Claude Code, Codex, OpenCode); a seat on another harness needs the policy changed first.

## Resource Profile

Slice 3's figures are run 1's measurements and Slice 2's are revision 6's estimates. The main capture is an estimate from the pilot's rate (133 runs in 44.9 s at eight workers, about 12 MB a run); the emulator times are from the planning probe under [Frozen Facts](#frozen-facts-and-evidence). Run 1 took 3 h 35 m 36 s for Slices 1 to 4 (its run report); Developer time for Slices 5 to 8 is not estimated.

| Slice | Mimic evaluations | Machine time | Disk |
|---|---|---|---|
| 2 (complete) | about 6, in tests | under a minute | about 70 MB, in a temporary directory |
| 3 (complete) | 133 | 44.9 s | 1.5 GB |
| 5 | none | the count control adds 15 emulator fits to the suite: about 1 to 2 minutes, estimated from the probe's per-fit times (a Gaussian-process fit at 400 points 3 to 8 s, a Bayes linear fit with the noise term 1.5 to 6 s) | none |
| 6 and 7 | none | Slice 6's two stage controls fit 24 emulators at 400 points and predict at 100,000 points: about 2 minutes, estimated from the round-2 probe; the rest of the fits in tests is not measured | none |
| 8 | 600 | about 3.5 minutes for the capture; the analysis (some hundreds of fits at up to 400 points) is not measured and is expected to take minutes | about 7 GB |

---

## Slice 1: Declared inputs: spec, constraint table and design

**Status:** Completed in run 20261110T090410Z-ea3675 at `47d7671`. The contract below is revision 6's, unchanged.

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
| Design parameters, each with low, high, the template's default and a scale (`log` for all five; `linear` is the other value), sampled uniformly in that scale | `SfrEfficiency` 0.01, 0.25, 0.05; `FeedbackReheatingEpsilon` 0.6, 10.0, 3.0; `FeedbackEjectionEfficiency` 0.06, 1.5, 0.3; `ReIncorporationFactor` 0.03, 0.75, 0.15; `RadioModeEfficiency` 0.016, 0.4, 0.08 |
| Inert control | `InertControl` 0.1, 10, 1.0, log scale: sampled like a parameter and marked inert, so no run file carries it |
| Design sets and seed | `pilot`: one row at every default, the 32 corners of the five real parameters (inert control at its default), then 100 Latin-hypercube points; `train`: 400 Latin-hypercube points; `holdout`: 200 Latin-hypercube points; seed 20261110 |
| Output statistic | a mass function of one named galaxy property: `StellarMass`, over galaxies where it is positive, at snapshot 63, in 0.1 dex bins centred on the constraint's converted masses |
| Output selection rule | bin centre at least 9.0; constraint `sigma_phi < phi`; at least 10 galaxies in at least 95 of the pilot's 100 Latin-hypercube runs. Exploratory outputs: bins that meet the first two conditions, fail the third, and hold at least 10 galaxies in the pilot's default row |
| Core and exploratory outputs | both empty in this slice (Slice 3 fills them) |
| Constraint | `constraints/baldry2008_smf.csv`, with its conversion declared in the spec: mass shifted by -0.26 dex and by h to the power -2, phi by h to the power 3 (so mass `x - 2 log10(h) - 0.26` and `phi h^3`), with h taken from the run |
| Discrepancy budgets | 0.05, 0.075, 0.1, 0.15, 0.2 and 0.3 dex; gate budget 0.1 dex |
| Implausibility rule | reject when the second-largest implausibility exceeds 3 |
| Criteria | per output, at least 0.90 of holdout standardised errors within 2, at most 0.05 beyond 3, and a root-mean-square standardised error of at least 0.5; calibrated, at least 0.75 of core outputs validated; false-exclusion rate over runs at most 0.05, measurable only with at least 20 holdout runs kept by the simulator; non-implausible fraction at the gate budget at most 0.75 |

### Acceptance Criteria
- [ ] Loading the spec fails, with a message naming the offending key and exit status 2 from the command line, on each of: an unknown key at any level; an item from the table above that is missing; a range with `low >= high`, a scale that is neither `log` nor `linear`, a non-positive bound on a log-scale parameter, or a default outside the range; a duplicate parameter name; a set size that is not a positive integer; a gate budget that is not one of the scanned budgets; a criterion fraction outside (0, 1]; a minimum of kept runs that is not a positive integer.
- [ ] `design` writes one row per evaluation, with an identifier, its set and every parameter value including the inert control: 133 pilot rows, 400 train rows and 200 holdout rows, values written so that reading them back gives the same floats, and two invocations on one spec write byte-identical files.
- [ ] In every Latin-hypercube set each dimension is stratified in its parameter's scale (each of the n equal-probability intervals holds exactly one point) and every value lies strictly inside its range; a test spec with a linear-scale parameter whose range spans zero loads and is stratified linearly; the three sets come from distinct random streams derived from the one seed; the pilot's first row holds every default, and its next 32 rows are the corners.
- [ ] `constraints.py` returns, for a given h and the spec's conversion, each point's converted bin centre, the logarithm of the converted density (`log10(phi h^3)` under the shipped spec's conversion), the standard deviation in dex `sigma_phi / (phi ln 10)`, and a flag where `sigma_phi >= phi`; at h = 0.73 the first centre equals `7.05 - 2 log10(0.73) - 0.26` and exactly the three highest-mass points are flagged.
- [ ] With `MIMIC_ROOT` set, a test parses the `baldry` array literal out of the figure source and finds the CSV's 50 rows equal to it at float32 precision; with `MIMIC_ROOT` unset that test skips and says why.
- [ ] No module names a model, a parameter, a galaxy property or a publication: `grep -rn -i -E 'sage16|SfrEfficiency|baldry|StellarMass' mimic_emulator/` prints nothing.
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

**Status:** Completed in run 20261110T090410Z-ea3675 at `d8e8798`. The contract below is revision 6's, unchanged; the main capture it calls Slice 7 is now Slice 8.

### Intended Change
- Add `runfile.py`: build a run file from a template and one design row by loading the template with PyYAML, setting `output.output_directory` to an absolute per-run directory, applying the spec's overrides and the row's real parameters under `modules.parameters`, and emitting it with key order kept and every scalar it does not set left as the template wrote it.
- Add `statistics.py`: galaxy counts per candidate bin for one finished run, and the output selection rule as a pure function of a capture's counts.
- Add `capture.py` and the `capture` stage: `python -m mimic_emulator capture --spec <spec> --mimic-root <dir> --sets <names> --out <dir> [--workers 8]` runs every evaluation of the named sets, verifies each, records its counts, and ends with a manifest.
- Add the `select-outputs` stage: `python -m mimic_emulator select-outputs --spec <spec> --capture <dir>` prints the core and exploratory outputs the spec's rule gives for a pilot capture, with the per-bin counts behind them.

### Acceptance Criteria
- [ ] Read with a loader that keeps every scalar as its written text (PyYAML's `BaseLoader`), a generated run file equals the template in every scalar except `output.output_directory`, the spec's overrides and the design row's real parameters, which carry the generated values; an empty value stays empty; the inert control never appears in it; a design parameter that the template's `modules.parameters` lacks, or a spec default that differs from the template's value, stops `capture` with exit status 2 before any run.
- [ ] `capture` runs each evaluation as `./mimic -q <run file>` with the Mimic root as working directory, in its own new directory under `<out>/runs/`, with stdout and stderr in a per-run log and a 120 s timeout; it refuses an `--out` that exists, and writes nothing outside `--out`.
- [ ] `capture`'s workers are threads (`concurrent.futures.ThreadPoolExecutor`), each waiting on one `./mimic` subprocess, so that the stage and its tests run inside the PM's OS sandbox: `ProcessPoolExecutor` fails there, because the `os.sysconf("SC_SEM_NSEMS_MAX")` call behind it is denied (project-manager's `references/sandbox.md`, *Troubleshooting*). `grep -rn ProcessPoolExecutor mimic_emulator/ tests/` prints nothing.
- [ ] A run counts as verified only when all hold: exit status 0; `metadata/version_info.json` exists; the `RunProperties/Parameters` of the run's master file (`<output_filename>.hdf5`) has exactly the template's parameter names in order, each value parsing to the float the run file carried; `ModelName` and `SimulationName` equal the spec's; `FirstFile` and `LastFile` equal the simulation's full range in `simulations/<simulation>/simulation_info.yaml`; `SubSteps` equals the template's; the spec's snapshot is present. A test shows verification failing when the expected row differs from what ran.
- [ ] Counts are over galaxies of every type whose spec-named property is positive, with mass `log10` of that property converted to solar masses through Mimic's own `output_schema.mass_to_msun` and the run's schema unit label (a unit label it does not handle stops `capture` with exit status 2), read through `hdf5_reader.read_hdf5_snapshot`, both imported from `<mimic-root>/plot/mimic-plot` with bytecode writing disabled, so that nothing is written in the checkout; the candidate bins are every constraint point's converted centre plus and minus 0.05 dex, with h from the run.
- [ ] Known answers, run against the real executable: the evaluation at every default has 36,530 rows at snapshot 63, and 920 and 392 galaxies in the candidate bins of the constraint's 9.05 and 10.45 rows (measured by the probe before the plan); its counts on the edges `numpy.linspace(8.0, 13.0, 51)`, turned into `N h^3 / (BoxSize^3 * 0.1)`, equal Mimic's `calculate_mass_function(mass, BoxSize**3, h, 0.1, 8.0, 13.0)` to 1e-12 relative; and two evaluations of one design row give identical counts.
- [ ] `<out>` holds `context.json` (the command line, package version and commit, spec path and SHA-256, the constraint file's SHA-256, Mimic root, the Mimic version and commit the runs reported, the `BoxSize` and `Hubble_h` they reported, the SHA-256 of `./mimic`, of the template and of every other Mimic file `capture` reads (the simulation's `simulation_info.yaml` and the two imported helpers), host, start and end times), `design.csv`, `runs.csv` (per run: set, exit status, wall time, verified, rows at the spec's snapshot), `outputs.csv` (per run: the count in every candidate bin) and `manifest.sha256` listing every other file under `<out>`; `shasum -a 256 -c manifest.sha256` passes.
- [ ] `capture` exits 0 only when every evaluation verified and all reported one Mimic version and commit and one `BoxSize` and `Hubble_h`; otherwise it still writes every file, records the failures in `runs.csv`, and exits 2.
- [ ] Second-model dry run, with no executable: a test builds, in the test, a spec for `sage26` on mini-Millennium with one log-scale parameter (`SfrEfficiency`) and one linear-scale parameter whose range starts at zero (`FracZleaveDisk`), and from `models/sage26/input/sage26_mini-millennium.yaml` writes a design and a run file that meets the first criterion, `MShockMsun` still reading `6.0e11`.
- [ ] `grep -rn -i -E 'sage16|SfrEfficiency|baldry|StellarMass' mimic_emulator/` prints nothing.
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
- Tests to add/update: run-file generation against the shipped template and, as a dry run, a second model's; verification, positive and negative; the statistic's known answers; capture end to end on a small design, including the refusal of an existing directory and the manifest; the selection rule.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` (no skips); `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`; `git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot`
- Lint (differential, via the `lint` skill): required.
- Manual checks: print one generated run file beside the template and read the differing keys.

### Rollback Path
- Revert the slice's commit; test captures live in temporary directories.

---

## Slice 3: Pilot capture and the frozen outputs

**Status:** Completed in run 20261110T090410Z-ea3675 at `0947d42`. The contract below is revision 6's, unchanged.

**As executed.** The pilot froze 16 core outputs, 9.05 to 10.55, and 8 exploratory ones, 10.65 to 11.35. The contract's "Tests allowed or expected to change: none" could not hold beside its own run of the suite after the freeze, because a Slice 1 test asserted that both output lists were empty. The Developer stopped uncommitted; the PM granted one path, `tests/test_spec.py` (run 1's plan decision D2), and that assertion now states the frozen lists. The grant made the slice elevated, so it was drift-audited and reviewed by the code panel, and its "Lint: not required" no longer applied. The spec the pilot recorded is the one before the freeze; Slice 5 changes the spec once more, before the main capture, by adding the noise model and nothing else.

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

**Status:** Completed in run 20261110T090410Z-ea3675 at `56d7115`, after five review rounds. The contract below is revision 6's, unchanged; the slices it calls 5 and 6 are now 6 and 7.

**As executed.** The known-good control's criterion names no draw and no tolerance across draws. The owner's reading (2026-11-10): the criteria are asserted on one pinned draw, and the rate over further draws is recorded and not gated, as a finding of the spike. The findings are Decision 16's and are measured under [Frozen Facts](#frozen-facts-and-evidence): the Bayes linear emulator meets the control on 69 of 90 further draws and the Gaussian process on 89 of 90, the Bayes linear failures being accurate and about 15% over-confident in standard deviation. Term selection is forward stepwise by the leave-one-out error of the least-squares fit, under effect heredity and a one-standard-error rule; the correlation length and nugget are chosen by a closed-form leave-one-out log predictive density. The measured draw counts are written by hand in the docstrings of `mimic_emulator/emulator.py` and `tests/test_emulator.py`, and they stay there; `evidence/analyses/2026-11-11-control-draw-sets/control_draw_sets.out` reproduces them, and the report of Slice 8 cites that file.

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

## Slice 5: Count-noise variance

### Intended Change
- Give both emulators an optional known noise variance per training point: `fit(x, y, noise_variance=None)`, with `noise_variance` an array of shape `(n,)` in output units squared. With `None`, or with a `noise_variance` that is zero at every point, each emulator takes Slice 4's path and is exactly what Slice 4 built; everything below that says "with noise" means a `noise_variance` that is positive at some point. `predict` keeps its signature. After a fit with noise its variance is the emulator's own: that of the response at `x` with the declared noise left out and the fitted nugget or white-noise term included. The caller adds the declared noise of a new evaluation.
- **Gaussian process.** The declared variance goes on the diagonal of the training covariance: scikit-learn's per-point `alpha = noise_variance / y_scale^2`, added to the `1e-10` jitter the Slice 4 fit already uses, with `y_scale` the standard deviation the output is standardised by. The white-noise kernel stays and is fitted as before, as a constant floor under the declared noise.
- **Bayes linear.** The residual covariance becomes `sigma^2 [(1 - nu) C + nu I] + diag(noise_variance)`, which is `sigma^2 R(sigma^2)` with `R(sigma^2) = (1 - nu) C + nu I + diag(noise_variance) / sigma^2`. Terms are selected as in Slice 4. At each correlation length and nugget of Slice 4's grids, `sigma^2` is the fixed point of `sigma^2 = GRSS(sigma^2) / (n - p)`, with GRSS the generalised residual sum of squares under `R(sigma^2)`. Coefficients, mean and variance are Slice 4's universal-kriging forms under that matrix.
- **The score that chooses the length and nugget.** With noise it is the leave-one-out log predictive density under the fixed full-fit covariance `K = sigma^2 [(1 - nu) C + nu I] + diag(noise_variance)`, with `sigma^2` at its fixed point: each observed output is scored against its universal-kriging prediction from the others, in closed form, with the coefficients re-estimated without the point and `sigma^2` held. `sigma^2` is not re-estimated without the point, because that would rescale the declared noise with it. With `None` or all zeros the fit takes Slice 4's path, so the score is Slice 4's, in which `sigma^2` is re-estimated without each point, bit for bit. The two scores are different objectives and are never mixed: which one applies is decided by whether any declared noise is positive.
- **The fixed point and its stopping rule.** `GRSS(s) / s` falls as `s` rises, and `GRSS(s)` never exceeds its value with no noise term, so the fixed point is unique when it exists and lies at or below `s_0`, the residual variance of the same fit without the noise term. It is found by a bracketed root search (Brent's method, `scipy.optimize.brentq`) in `t = log s`, for the root of `log(GRSS(e^t) / (n - p)) - t` over `[log s_floor, log s_0]`, where `s_floor` is `1e-12` times the sample variance of the training outputs. The search stops when the bracket is narrower than `1e-10` in `t`. When that function is not positive at `s_floor`, or `s_0` is not above `s_floor`, the declared noise alone accounts for the residuals and `sigma^2 = s_floor`. When it is not negative at `s_0`, which can happen only by rounding, `sigma^2 = s_0`. A grid point at which a factorisation fails is skipped, as in Slice 4.
- Add `leave_one_out()` to both emulators: for each training point, the residual of its output from the prediction made without it, and the variance of that prediction, with the fitted hyperparameters, residual variance and output standardisation held, and the point's own declared noise included in the variance. For a Bayes linear fit with noise these are the quantities the selection score is made of.
- Extend `describe()` on both emulators. Every key it has keeps its name and meaning. Both add whether a declared noise entered the fit, which is false for `None` and for all zeros. The Bayes linear emulator adds whether its residual variance sits at the floor, and with noise its `loo_log_density` is the fixed-covariance score above. New fitted state these need may be initialised in `__init__`.
- Names and signatures that stay. `noise_variance` is the name of the new `fit` argument on both emulators. The Gaussian process already has a property of that name, its fitted white-noise variance, which Slice 4's tests read; the property keeps its name and meaning, and the docstrings of the argument and of the property say which is which. Slice 4's tests also import the private helpers `_gls_fit`, `_gls_predict` and `_loo` and call them positionally, so any parameter this slice adds to them is a trailing keyword with a default that reproduces Slice 4's behaviour.
- Add a required `noise_model` section to the spec and its loader: a `kind`, of which `poisson_log10` is the one supported, and a positive `scale`. The loaded noise model carries a function of counts that returns the variance `scale (log10 e)^2 / max(N, 1)`, in dex squared. Add the section to `specs/sage16-smf-spike.yaml` with `scale: 1.0` and a comment that states the formula and its units.
- Extend the module docstring of `emulator.py` with the noise path: the two covariances, the fixed point and its stopping rule, the score that chooses the length and nugget with and without noise, what `predict` returns after a fit with noise, and `leave_one_out()`.

### Acceptance Criteria
- [ ] `tests/test_emulator.py` is byte-identical to its state at `56d7115` (`git diff --quiet 56d7115 -- tests/test_emulator.py` exits 0) and every test in it passes; the emulator tests this slice adds are in new files.
- [ ] With no `noise_variance`, each emulator's mean and variance on the pinned control draw of `tests/test_emulator.py` (its `control_data` fixture) are bit-identical to those of the module as committed at `56d7115`, and so is the Bayes linear emulator's `loo_log_density`: the score with no noise is Slice 4's, with `sigma^2` re-estimated without each point. `validation.md` records the comparison and how it was made.
- [ ] A `noise_variance` of zeros takes the no-noise path: for both emulators its means, variances and `describe()` output are bit-identical to those with `None`.
- [ ] `fit` raises a `ValueError` that names the cause when `noise_variance` has the wrong shape, a negative value or a non-finite value; `predict`'s signature is unchanged.
- [ ] Gaussian process, independent check: on a fixed case of at most 40 points with a `noise_variance` that differs between points, `predict` at new points and at training points equals a dense evaluation written in the test from the hyperparameters `describe()` reports: training covariance `signal_variance * RBF + (fitted white-noise variance + 1e-10 y_scale^2) I + diag(noise_variance)`, cross-covariance `signal_variance * RBF`, and prior variance of the predicted point `signal_variance +` the fitted white-noise variance, on the output less its mean. The means agree to 1e-6 relative. The variances agree to 1e-6 of the signal variance after the dense value is floored at the fitted white-noise variance, as `predict` floors its own (the floor acts only on rounding). So the declared noise is in the training covariance and absent from the predicted variance.
- [ ] Bayes linear, independent check: on a fixed small case (at most 30 points, fixed terms, correlation length and nugget, a `noise_variance` that differs between points) the mean and variance equal, to 1e-8 relative, a dense evaluation written in the test of universal kriging under `K = s [(1 - nu) C + nu I] + diag(noise_variance)`, with `s` the emulator's reported residual variance: `beta = (H' K^-1 H)^-1 H' K^-1 y`, mean `h' beta + k' K^-1 (y - H beta)`, variance `s - k' K^-1 k + u' (H' K^-1 H)^-1 u`, where `k = s (1 - nu) c(x)` and `u = h - H' K^-1 k`.
- [ ] On that case the reported residual variance satisfies the fixed-point equation, evaluated densely in the test, to 1e-6 relative. On a case whose declared noise exceeds the scatter of the outputs about the fitted terms, the fit succeeds with the residual variance at the floor, `describe()` says so, and every predicted variance is finite and not negative.
- [ ] The selection score with noise is the fixed-covariance one, and it applies only when some declared noise is positive. On a small fixed case with noise and fixed terms, `describe()`'s `loo_log_density` equals, to 1e-9 relative, the sum over training points of the Gaussian log density of `leave_one_out()`'s residual under its variance; and with the length and nugget left free, no pair on the grids, fitted with that pair fixed, has a larger `loo_log_density` than the fit the emulator chose (a pair whose fit raises is skipped).
- [ ] `leave_one_out()` on each emulator returns residuals and variances of shape `(n,)`. On a small fixed case, with and without `noise_variance`, they equal a dense computation written in the test that removes each point in turn and predicts it from the others with the fitted hyperparameters, residual variance and output standardisation held, the removed point's own declared noise added to the variance: to 1e-8 relative for the Bayes linear emulator and 1e-6 for the Gaussian process. For the Bayes linear emulator the dense computation is universal kriging under `K` with the point's row and column removed: the coefficients are re-estimated without the point and `sigma^2` is not, so the declared noise is never rescaled.
- [ ] With `noise_variance` given, each emulator's predicted variance is finite, never negative, and at a training point at least its nugget variance or fitted white-noise variance; two fits on the same data give identical predictions and identical `describe()` output. `describe()` keeps every key Slice 4 gave it, says on both emulators whether a declared noise entered the fit, and says on the Bayes linear emulator whether the residual variance is at the floor.
- [ ] Loading a spec fails, with a message naming the offending key and exit status 2 from the command line, on each of: a missing `noise_model` section; an unknown key in it; a `kind` other than `poisson_log10`; a `scale` that is not a finite positive number. The shipped spec loads with kind `poisson_log10` and scale 1.0.
- [ ] The noise model's function of counts returns `scale (log10 e)^2 / max(N, 1)` element by element: `0.188611697011614 scale` at N = 0 and at N = 1, and a hundredth of that at N = 100, to 1e-12 relative. It raises on a negative or non-finite count.
- [ ] `git diff 56d7115 -- specs/sage16-smf-spike.yaml` shows the added `noise_model` section with its comments and no other change: the core and exploratory lists and every other value are as Slice 3 left them.
- [ ] Count control with the noise term (specified under Inputs): pooled over the five draws, each emulator has at least 0.90 of standardised errors within 2, at most 0.05 beyond 3, a root-mean-square standardised error between 0.5 and 1.5 over all test points and between 0.7 and 1.3 in each of the four count bands; and on each draw its root-mean-square error against the noise-free function is under 0.2 of that function's standard deviation over the unit cube.
- [ ] Count control without the noise term: the Gaussian process fitted to the same five draws with no `noise_variance`, its variance used as `predict` returns it, has a pooled root-mean-square standardised error above 1.5 in the band below 30 galaxies.
- [ ] The control's function, draws, bands and thresholds are those of this slice, and none is changed to obtain a pass. `validation.md` records, for each draw and pooled, for both emulators with the term and for the Gaussian process without it: the fractions within 2 and beyond 3, the root-mean-square standardised error over all points and in each band, and the number of test points in each band. The values for single draws are recorded and not asserted.
- [ ] Any parameter added to `_gls_fit`, `_gls_predict` or `_loo` is a trailing keyword with a default, and the Gaussian process's `noise_variance` property is still its fitted white-noise variance.
- [ ] The module docstring states the noise path as Intended Change lists it, and its record of the Slice 4 control findings is unchanged; `grep -rn -i -E 'sage16|SfrEfficiency|baldry|StellarMass' mimic_emulator/` prints nothing.
- Inputs, the count control:
  - The expected count is `lambda(x) = 5 * 400^L(x)`, with `L(x) = 1 / (1 + exp(-(f(x) - 0.7) / 0.5))` and `f` the control function of Slice 4's tests (`control_function` in `tests/test_emulator.py`: five active inputs, the sixth inert). It runs from about 5 to about 2,000 and is mostly high: over the unit cube about 11% of points have an expected count below 30 and 52% one of 300 or more.
  - The draw with seed `s` takes, from one generator `numpy.random.default_rng(s)` and in this order: 400 training inputs and 200 test inputs (the tests' `latin_hypercube`), the training counts and the test counts (`Generator.poisson` of the expected counts). The draws are seeds 7000, 7001, 7002, 7003 and 7004, each fitted by itself; statistics are pooled over their 1,000 test points.
  - The emulated output is `y = log10(max(N, 1))`. The training noise is the spec function's variance, at scale 1, of each training count. At a test point the variance of a new evaluation is the emulator's variance plus that function's value at the predicted count `10^E`, E being the predicted mean. A standardised error is the observed test `y` less E, over the square root of that variance. The noise-free function is `log10 lambda`; its standard deviation over the unit cube is estimated as Slice 4's tests estimate theirs (about 0.63 dex).
  - The count bands are by the test point's observed count: below 30; 30 to below 100; 100 to below 300; 300 and above.
  - Context, not a requirement: a planning probe written to this contract (in `evidence/analyses/2026-11-11-count-control-probe/`) measured, pooled over these five draws, 0.938 within 2, 0.008 beyond 3, 1.06 overall and 1.06, 1.11, 1.11 and 1.03 in the bands for the Gaussian process with the term; 0.943, 0.007, 1.04 and 1.05, 1.09, 1.07 and 1.01 for the Bayes linear emulator; and 2.12 below 30 for the Gaussian process without it. The bands hold 98, 190, 196 and 516 of the 1,000 test points. Statistics are pooled because one draw puts as few as 13 test points in a band, where sampling alone takes a calibrated emulator outside 0.7 to 1.3: on single draws all four bands held on 21 of 30 draws for the Gaussian process and 24 for the Bayes linear emulator.
- Inputs, general: inputs lie in the unit cube, outputs are finite and `noise_variance` is finite and not negative; behaviour outside that is unspecified, apart from the rejections named above. Sizes up to 400 training points, 6 inputs and 30 outputs fitted one at a time. The interpreter `.venv/bin/python`; `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic` exported for the test suite. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Stop: if the count control is not met by an implementation that meets every other criterion, the slice stops with a question to the PM that gives the measured values. That would be a finding about the noise model, not a defect to tune away.
- Behaviour that must not change: the no-noise behaviour (`noise_variance` `None` or all zero) of Slice 4's emulators, and every test of Slices 1 to 4, with the exceptions this slice's surface names: a test of Slices 1 to 3 that builds or enumerates a spec gains the new section, and `spec.py` and the shipped spec gain it. Every validation `spec.py` already makes is kept, and the stages of Slices 1 to 3 behave as before on a spec that carries the section.

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/emulator.py`
  - `mimic_emulator/spec.py`
  - `specs/sage16-smf-spike.yaml`
  - `tests/`
- Functions/classes/components allowed to change: in `emulator.py`, `fit` and what it calls (the private helpers included, by trailing keywords), `describe()` on both emulators, new fitted-state attributes in `__init__` on both, the new `leave_one_out()`, docstrings of what changes, and the module docstring's additions; in `spec.py`, the new section's dataclass and parsing and the `Spec` field that holds it; in the shipped spec, the new section only.
- Tests allowed or expected to change: new tests under `tests/` for the noise path, the count control and the new spec section; existing tests of Slices 1 to 3 only where a spec they build or enumerate must carry the new section; `tests/test_emulator.py` not at all.

### Explicit Non-Goals
- No use of the noise model by a stage: `design`, `capture`, `select-outputs` and the statistic are unchanged, and `analyse` is Slice 6.
- No estimate of the scale from data, no second noise kind, and no declared noise term inside `predict`: the fitted nugget or white-noise term stays in it, as in Slice 4. No change to term selection, the grids, the kernel, the restarts, the seeds, or the residual variance and the selection score when the noise is `None` or all zero.
- No change to the docstring's record of the Slice 4 control findings, and none to the core and exploratory lists or any other value of the shipped spec.
- No capture, no reading of `evidence/`, and no dependency beyond `requirements.txt`.

### Risk Flags
- Risky surfaces touched: numerics (the fitted variance whose honesty the spike measures); the frozen spec (a declared model added before the main capture)
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: the two dense checks; the fixed-point equation and the floor; the leave-one-out checks, for `leave_one_out()` and for the selection score; what `describe()` reports; zeros against `None`, bit for bit; the rejections of a malformed `noise_variance`; determinism with noise; the count control with and without the term; the spec section's rejections and the variance function's known answers.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs` (no skips); `git diff --quiet 56d7115 -- tests/test_emulator.py`; `git diff 56d7115 -- specs/sage16-smf-spike.yaml`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`; `grep -rn -i -E 'sage16|SfrEfficiency|baldry|StellarMass' mimic_emulator/`
- Lint (differential, via the `lint` skill): required.
- Manual checks: the bit-identity comparison, made by writing `git show 56d7115:mimic_emulator/emulator.py` to a temporary directory outside the repository, importing it under another module name, and comparing both emulators' predictions, and the Bayes linear `loo_log_density`, on the pinned control draw with `numpy.array_equal`; the wall time of the new emulator tests, recorded in `validation.md`.

### Rollback Path
- Revert the slice's commit; the emulators and the spec return to their state at `56d7115`, and nothing outside the repository was written.

---

## Slice 6: Diagnostics, implausibility and the verdict

### Intended Change
- Add `diagnostics.py` (standardised errors on a holdout and the per-output and per-configuration calibration results), `implausibility.py` (emulator and simulator implausibility, the rejection rule, the non-implausible fraction of the prior box) and `analyse.py` with the `analyse` stage: `python -m mimic_emulator analyse --spec <spec> --capture <dir> --out <dir>`.
- `analyse` forms `log10 phi` per core output from the captured counts (a zero count set to one first), with `BoxSize` and `Hubble_h` taken from the capture's `context.json`, scales the design to the unit cube in each parameter's own scale, fits each candidate configuration on the `train` set, tests it on the `holdout` set, and writes `verdict.json` and tables. The candidate configurations, in order, are the Bayes linear emulator and the Gaussian process, both on `log10 phi` and both with the spec's noise model.
- The noise model enters in two places. In the fit: each training run's `noise_variance` for an output is the spec's noise variance of that run's captured count in that bin. At a prediction point: the predicted count is `N_pred = 10^E * BoxSize^3 * w / h^3`, with E the predicted `log10 phi` and w the spec's bin width in dex (the inverse of `phi = N h^3 / (BoxSize^3 * w)`), and the spec's noise variance of `N_pred` is added to the emulator's variance. That total, `Var + v(N_pred)`, is the predictive variance in the standardised errors and in the emulator's implausibility. The simulator's own implausibility has no noise term.
- The functions that compute a criterion or the verdict are pure functions of arrays and the spec, separate from the stage function that reads and writes files. They take the training noise and the prediction-point noise as arguments, each of which may be `None`. The prediction-point noise depends on the predicted mean, so it is passed as the noise model or as a callable of E, not as an array. A later slice can then add outputs, and the same diagnostics for fits with no noise or with noise at prediction only, without touching them.
- `analyse` also writes, beside the verdict, a table of what each fit reports about itself: per configuration and core output, the fit's `describe()` (terms and active inputs, correlation length and nugget or length scales and white-noise variance, residual variance, whether it sits at the floor, optimiser warnings). Each Bayes linear row also carries two flags, whether the chosen correlation length and whether the chosen nugget sit at an end of their grids, which `analyse.py` works out from `emulator.CORRELATION_LENGTHS` and `emulator.NUGGET_FRACTIONS` without any change to `emulator.py`; each Gaussian-process row carries the fit's `fit_warnings`, which name a bound the optimiser reached. The table enters no criterion; it is there so that a fit at a grid end, a bound or the floor can be read from a file without a second analysis.

### Acceptance Criteria
- [ ] Each candidate is fitted, per output, with `noise_variance` equal to the spec's noise variance of each training run's captured count (a zero count has the variance of one galaxy). A test checks by hand that a bin holding N galaxies has `log10 phi = log10(N h^3 / (BoxSize^3 * w))` and that `N_pred` of that value is N.
- [ ] The pure functions accept `None` for the training noise and for the prediction-point noise, separately, and take the prediction-point noise as the noise model or a callable of the predicted mean. A test calls them with both noises, with neither, and with the prediction-point noise alone, on one small case, and the three predictive variances differ as they should: by `v(N_pred)` between the last two at equal fits.
- [ ] Per configuration and core output, `analyse` reports on the holdout, with each standardised error the holdout run's value less the predicted mean over `sqrt(Var + v(N_pred))`: the fractions of standardised errors within 1, 2 and 3, their mean and root mean square, the root-mean-square error in dex, and `validated` when the spec's three per-output criteria hold; an output whose emulator variance is non-finite or not positive at any holdout point is not validated; a configuration is `calibrated` when the spec's fraction of core outputs is validated.
- [ ] Implausibility follows this definition exactly: for output k, the emulator's is `|E - z| / sqrt(Var + v(N_pred) + sigma^2 + b^2)` in dex, with the constraint's converted value z and error sigma and the budget b; the simulator's replaces E by the run's own value and drops both Var and `v(N_pred)`, so it is `|y - z| / sqrt(sigma^2 + b^2)`; a point is rejected when the second-largest value over the configuration's validated core outputs exceeds 3. A test checks one hand-computed case of each, the emulator's with a count-noise term that changes its value. A configuration with fewer than two validated core outputs has no implausibility computed and cannot pass.
- [ ] The false-exclusion rate, at each budget, is counted over runs: among holdout runs the simulator keeps under the rejection rule on the configuration's validated outputs, the fraction the emulator rejects; `verdict.json` records its numerator and denominator. A test on constructed implausibilities has a rate over run and output pairs under 0.05 and a rate over runs above it, and the criterion fails. Reported beside the rate, and entering no criterion: the rate over run and output pairs, and the fraction of simulator-rejected holdout runs that the emulator also rejects.
- [ ] The non-implausible fraction, at each budget, is taken over 100,000 Latin-hypercube points of the whole design box, sampled as the design is (uniform in each parameter's scale) from a fixed seed, with the emulator's implausibility as defined above; beside it `analyse` reports the fraction of the 200 holdout runs the simulator keeps on the same validated outputs, with a binomial standard error.
- [ ] A configuration passes when it is calibrated, its false-exclusion rate at the gate budget is at most the spec's limit, and its non-implausible fraction at the gate budget is at most the spec's limit. The verdict is `pass` when at least one candidate passes, naming the first that does in candidate order and saying when it is not the primary; `fail` when none does; `error` when the capture has an unverified run, the SHA-256 of a file `analyse` reads differs from its manifest entry (entries for files it does not read are ignored), the spec's or the constraint file's SHA-256 differs from the one the capture recorded, the core outputs are empty, or the simulator keeps fewer holdout runs at the gate budget than the spec's minimum for a calibrated configuration (its false-exclusion rate cannot be measured).
- [ ] `analyse` exits 0, 1 or 2 for pass, fail or error, and `verdict.json` holds the same word, every criterion's measured value and threshold per configuration, the noise model's kind and scale as the spec declares them, the count of floored values, and the SHA-256 of the spec, the capture's manifest and the package commit.
- [ ] Controls, run through the `analyse` stage on the synthetic captures pinned under Inputs (no Mimic), whose counts are Poisson draws about a smooth expected count, so that the declared noise model is true of them: the known-good capture exits 0, with a calibrated configuration, at least the spec's minimum of holdout runs kept by the simulator at the gate budget, a false-exclusion rate within the spec's limit and a non-implausible fraction within the spec's limit; the known-bad capture exits 1 with the calibration criterion failing for both configurations; the known-good capture with one run marked unverified exits 2.
- [ ] `analyse` writes the table of each fit's `describe()`, one row per configuration and core output, beside `verdict.json`; a Bayes linear row flags a correlation length or a nugget at an end of its grid, from the grids `emulator.py` exports, and a Gaussian-process row carries its `fit_warnings`; a test checks both flags on a fit with a fixed length and nugget at a grid end and on one inside. `verdict.json` and the exit status do not depend on the table.
- [ ] `analyse` reads only `context.json`, `design.csv`, `runs.csv`, `outputs.csv` and `manifest.sha256` from the capture, plus the spec and its constraint file, and writes only under `--out`, which it refuses if it exists.
- Inputs, the stage controls. Both are captures built in the test, of the shape Slice 2's `capture` writes, for a spec made from the shipped one by keeping the first six of its core outputs (9.05 to 9.55), emptying its exploratory outputs and pointing its constraint at a table the test writes, with one row for each of the six outputs; nothing else in the spec changes, so the rows are the shipped design's `train` and `holdout` sets (400 and 200) and the budgets, the rule and the criteria are the shipped ones.
  - With u a row's coordinates in the unit cube (each parameter in its own scale, the sixth being the inert control), the expected count of core output j is `A_j * 10^(alpha_j * t(u))`, where `t(u) = (f(u) - 1.14) / 0.70`, f is the control function of Slice 4's tests (`control_function` in `tests/test_emulator.py`), `A = (400, 600, 900, 1300, 1900, 2800)` and `alpha = (0.30, -0.35, 0.40, -0.45, 0.50, -0.55)`.
  - The counts are `numpy.random.default_rng(8000).poisson(expected)`, with `expected` the array of shape (600, 6) whose rows are in design order, train then holdout.
  - The constraint's converted value for output j is the `log10 phi` of a count of `A_j`, and its error is 0.05 dex.
  - The known-bad capture is the same, from the same seed, with the holdout rows' expected counts multiplied by `10^(0.3 sin(2 pi u_1))`, u_1 being the first coordinate.
  - Context, not a requirement: a planning probe written to this contract (in `evidence/analyses/2026-11-11-count-control-probe/round2/`) measured on the known-good capture 6 of 6 outputs validated for both emulators and, at the gate budget, 82 of 200 holdout runs kept by the simulator, none of them rejected by either emulator, and a non-implausible fraction of 0.485; on the known-bad capture no output validated, with at most 0.13 of standardised errors within 2. Over the Poisson seeds 8000 to 8009 both configurations passed every time, with at most 2 false exclusions among 80 to 87 kept runs.
- Stop: if a pinned control does not give its exit status with an implementation that meets every other criterion, the slice stops with a question to the PM that gives the measured values; no function, seed or constraint of a control is changed to obtain it.
- Inputs, general: a capture written by Slice 2's `capture` (or a synthetic one of the same shape); the frozen spec with Slice 5's noise model; Slice 5's emulators through their interface (`fit` with `noise_variance`, `predict`, `describe`). The criteria, the thresholds and the verdict rule are the spec's, and the noise model changes none of them: it changes only the variance they are computed with. The interpreter `.venv/bin/python`. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: the existing stages and the tests of Slices 1 to 5.

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
- No change to `emulator.py` or `spec.py`: a defect found there is a question to the PM.
- No run of `./mimic`, no analysis of the pilot or main capture (Slice 8), no figures or supplementary analyses (Slice 7).
- No fit without the noise model in the verdict path (Slice 7 reports those), no estimate of the noise scale, and no count-noise term in the simulator's implausibility.
- No threshold, budget or rule outside the spec, no multivariate implausibility, no second wave.

### Risk Flags
- Risky surfaces touched: verdict logic (the outcome a campaign or the owner will act on)
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: diagnostics on arrays with known fractions; the count and `N_pred` hand case; the pure functions with both noises, neither, and the prediction-point noise alone; the hand-computed implausibility cases; the rejection rule with an unvalidated output excluded; the constructed false-exclusion case; the degenerate cases (fewer than two validated outputs, too few kept runs, a non-positive variance); the three controls; the table of fits; the refusals.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read `verdict.json` from the known-good control and confirm every criterion in the spec appears with its value, and the noise model with its kind and scale; record in `validation.md` the known-good control's validated outputs, kept runs, false exclusions and non-implausible fraction at the gate budget.

### Rollback Path
- Revert the slice's commit.

---

## Slice 7: Supplementary analyses and figures

### Intended Change
- Extend `analyse` to write, beside the verdict and changing nothing in it, eight supplementary results, each as a table under `<out>/tables/`:
  - a learning curve: the Slice 6 per-output diagnostics for each candidate refitted on 100, 200 and 400 training rows (nested fixed-seed subsets);
  - the fits without the noise model: for each emulator fitted with no `noise_variance` and judged with no count noise added at prediction, which is the emulator as Slice 4 built it, every Slice 6 diagnostic (per output; and per configuration the calibrated result, the false-exclusion rate with its numerator and denominator and the non-implausible fraction at each budget), laid out as the candidates' are;
  - the fits with noise at prediction only: the same fits with no `noise_variance`, judged with the count noise `v(N_pred)` added at prediction as it is for the candidates, with every Slice 6 diagnostic in the same layout. Each of the three kinds of configuration then has a Bayes linear and a Gaussian-process member: the candidates (noise in the fit and at prediction), noise at prediction only, and no noise;
  - calibration by count band: for each of the six configurations, over the core outputs pooled, in the bands of the holdout run's captured count (below 30; 30 to below 100; 100 to below 300; 300 and above), the number of run and output pairs, the root-mean-square standardised error and the fraction within 2;
  - the transformation comparison, every fit in it made without the noise model so that it shows what a transformation does by itself: the Bayes linear emulator on `arctan(log10 phi)`, and both emulators on `sqrt(N)` with N the captured count; for each, the calibration fractions and root-mean-square standardised error in the transformed space, and the error mapped back to dex;
  - the training leave-one-out diagnostic: per fit and core output, for the two candidates' fits and the two fits without the noise model, each row being that fit's own; no leave-one-out diagnostic is reported for the configurations with noise at prediction only. Per row, the root mean square of the leave-one-out standardised errors over the training rows, from `leave_one_out()`, and the fraction within 2;
  - the exploratory outputs: the Slice 6 per-output diagnostics, with the noise model, for the spec's exploratory outputs;
  - per-parameter profiles: for the passing configuration (or the primary when none passes), the non-implausible fraction at the gate budget in 20 equal slices of each design dimension, the inert control included (reported as not computed when that configuration has no implausibility).
- Add `figures.py`: standardised-error distributions and predicted-against-run values per configuration, the non-implausible fraction against budget with the simulator's own estimate, the learning curve, and the profiles, written as PNG files under `<out>/figures/`.

### Acceptance Criteria
- [ ] The verdict path is unchanged: Slice 6's tests pass without modification, and the slice changes no line that computes a criterion or the verdict; the stage function may gain calls made after `verdict.json` is written.
- [ ] Each of the eight tables is written with a header naming its quantities and units and saying that its rows enter no criterion, and each figure is drawn from a table, never from a second computation.
- [ ] The fits without the noise model are computed by Slice 6's functions, called with no training noise and no prediction-point noise, and the configurations with noise at prediction only by the same functions with no training noise and the spec's noise model at prediction. A test on the known-good control shows that the two share their emulator variance at every holdout point, that their predictive variances differ there by `v(N_pred)`, and that both differ from the candidates'.
- [ ] The count-band table has all six configurations; in it the pair counts of a configuration sum to the holdout runs times the core outputs, and a test with constructed standardised errors and counts checks one band's root mean square and fraction by hand, with a count on a band edge falling in the upper band.
- [ ] For `sqrt(N)` the emulated value is the square root of the captured count, zero for an empty bin, and the calibration is against the square root of the holdout count. The dex error is the holdout run's `log10 phi` less the `log10 phi` of the count `max(m, 1)^2`, m being the predicted mean; a test checks that a predicted mean of 10 maps to the `log10 phi` of 100 galaxies.
- [ ] The training leave-one-out table's value for a configuration and output equals the root mean square of `residual / sqrt(variance)` from that fit's `leave_one_out()`, and the table has a row for each of the four fits and every core output, and none for the configurations with noise at prediction only.
- [ ] On the known-good control the inert control's profile is flat (every slice within 0.05 of its mean) and at least one active input's profile spans more than 0.2.
- [ ] The learning curve uses nested subsets (each smaller set inside the next) and reports the configuration's `calibrated` result and median root-mean-square error at each size; a size at which a fit raises is reported as not fitted, with the cause.
- [ ] Figures render with the non-interactive Agg backend and with `MPLCONFIGDIR` set to a temporary directory outside the repository, which the stage removes. The variable is set before matplotlib is first imported in the process that renders (in `figures.py` ahead of the import, or by rendering in a subprocess), because matplotlib reads it once, at import. Axis labels carry units. The stage writes nothing outside `--out` and that temporary directory, so no font cache lands under `--out`; the test that checks this runs the stage in a fresh process.
- Inputs, the profile criterion. The known-good control is Slice 6's pinned one. Context, not a requirement: a drafting check of that control's noise-free expected counts, with no fitted emulator and no saved output, gave at the gate budget an inert-control profile within 0.013 of its mean and one active input (the fourth) spanning 0.295; the other four active inputs span 0.07 to 0.17. Only one input clears 0.2, so the margin of the second half of the criterion is thin.
- Stop: if the known-good control does not meet the profile criterion (the inert control's profile not flat within 0.05, or no active input spanning more than 0.2) with an implementation that meets every other criterion, the slice stops with a question to the PM that gives the measured profiles. Neither the control nor a threshold is changed to obtain it.
- Inputs, general: Slice 6's `analyse` and its outputs; Slice 5's emulators through their interface, `leave_one_out()` included. The interpreter `.venv/bin/python`. Style: black, isort and ruff as `pyproject.toml` configures them; module and function docstrings as `/Users/dcroton/Local/git-repos/mimic/docs/STYLE-GUIDE.md` asks of Python (*Python Docstrings*, *Python Style Beyond Formatting*).
- Behaviour that must not change: every criterion, threshold and verdict of Slice 6; the modules and tests of Slices 1 to 5.

### Authorized Surface
- Files allowed to change:
  - `mimic_emulator/analyse.py`
  - `mimic_emulator/figures.py` (new file)
  - `tests/`
- Functions/classes/components allowed to change: in `analyse.py`, additions that write supplementary tables and call `figures.py`; the verdict path is not edited.
- Tests allowed or expected to change: new tests under `tests/` for the eight tables and a smoke test that the figures are written.

### Explicit Non-Goals
- No new criterion, no change to how a criterion is computed, and no supplementary result feeding the verdict: a configuration outside the candidates that would have passed, or a transformation that calibrates better, changes no verdict.
- No interactive plotting, no projection figures beyond the one-dimensional profiles, no notebook, and no figure beyond those listed.
- No change to `emulator.py`, `spec.py`, `diagnostics.py` or `implausibility.py`: a defect found there is a question to the PM.

### Risk Flags
- Risky surfaces touched: none
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the unchanged-verdict check; the fits without the noise model and with noise at prediction only; the count-band hand case with six configurations; where `MPLCONFIGDIR` points and that no font cache is left under `--out`, checked in a fresh process; the `sqrt(N)` mapping; the leave-one-out table; the profile known answer; nested subsets; table headers; the figure smoke test.
- Commands to run: `MIMIC_ROOT=/Users/dcroton/Local/git-repos/mimic .venv/bin/python -m pytest -q -rs`; `.venv/bin/black --check .`; `.venv/bin/isort --check-only .`; `.venv/bin/ruff check .`
- Lint (differential, via the `lint` skill): required.
- Manual checks: open each figure from the known-good control once.

### Rollback Path
- Revert the slice's commit; Slice 6's stage is unchanged underneath.

---

## Slice 8: The main capture, the analysis and the report

### Intended Change
- Capture the `train` and `holdout` sets once (`--sets train,holdout --out output/main`), commit the capture's small files under `evidence/main/`, run `analyse` on that committed copy into `evidence/main/analysis/`, and write `reports/SPIKE-REPORT.md`.
- Complete `README.md`: what the package is, its stages and exit statuses, the spec with its noise model, how to reproduce the analysis from `evidence/` without Mimic, what a caller must supply to run it as an evidence node, what a spec for another model package must declare and what the package cannot yet do for one, and where a capture's manifest can be verified.

### Acceptance Criteria
- [ ] The main capture ran once from a clean worktree and exited 0 with 600 of 600 runs verified; its Mimic version and commit and every Mimic SHA-256 in its `context.json` equal the pilot's in `evidence/pilot/context.json`; the spec SHA-256 it recorded is that of the spec as Slice 5 left it; `shasum -a 256 -c manifest.sha256` passes in `output/main`; `evidence/main/` holds the five small files byte-identical to the capture's.
- [ ] `analyse` ran once on `evidence/main/` with the frozen spec and exited 0 or 1; the report's verdict line equals `verdict.json`, and an exit of 2 stops the slice with a question to the PM.
- [ ] Every figure the report quotes resolves to a committed measurement file in this repository: a named file under `evidence/`, the analyses under `evidence/analyses/` among them, or `reports/PILOT-RECORD.md`, which Slice 3 committed and reviewed. Every table in the report names the file it was copied from. No figure is quoted from a docstring, a test's output, a planning document or a record outside the repository.
- [ ] The report quotes a figure for the count control only from `evidence/analyses/2026-11-11-count-control-probe/` or from a file a slice wrote under `evidence/`. It says that those figures are a planning probe's, made before the package had the noise term, and that the package's own test asserts the control's thresholds on the same five draws, citing the test by path and package commit.
- [ ] The report has, in order: the verdict and what it supports (writing a plan for the instrument, or stopping); what was measured (Mimic version and commit, package commit, design sizes, wall time, and the spec's SHA-256 with its history: as the pilot recorded it, after the outputs were frozen, and after the noise model was declared, which is the one the main capture recorded); the frozen outputs, from `reports/PILOT-RECORD.md`; the noise model (what is declared, that it was declared and not fitted and when, the pilot evidence for it, and what the pilot could not show); calibration per configuration and output; calibration by count band; the false-exclusion rate with its numerator and denominator, and the emulator's power against the simulator's own classification; the non-implausible fraction at every scanned budget, beside the simulator's estimate, with how far the removal criterion was from binding; the profiles, with the inert control's result stated; the learning curve; the comparison of the two emulator families, with the run-1 control finding, the training leave-one-out diagnostic and which fits sit at a grid end, a bound or the residual-variance floor (from the flags and warnings in Slice 6's table of fits, cited by file), the evidence and a recommendation; the fits with noise at prediction only and the fits without the noise model, each beside the candidates; the comparison of output transformations (`arctan` and `sqrt(N)`), with the evidence and a recommendation; the exploratory outputs; observations on how the result depends on the discrepancy budget and on what a campaign's stored state consists of, labelled as observations; limits; and what a plan for the instrument must take from this.
- [ ] The run-1 control finding is stated with its measured numbers and its caveat: the draw counts of the three sets for the Bayes linear emulator with selected terms and with all 84 terms and for the Gaussian process, that the suite asserts the control on one pinned draw and records the rate, the character of the failures, and that the all-84-terms fits make the inert input active, so they are not a remedy. The report cites the draw counts and the failing draws from `evidence/analyses/2026-11-11-control-draw-sets/control_draw_sets.out` and the other figures of the finding from `evidence/analyses/2026-11-10-bl-calibration-review/`. It notes that the same draw counts are written by hand in the docstrings of `mimic_emulator/emulator.py` and `tests/test_emulator.py`, which this slice does not edit.
- [ ] The fits without the noise model, the fits with noise at prediction only, the count-band table, the `sqrt(N)` comparison and the training leave-one-out diagnostic are each reported from Slice 7's tables and each said to enter no verdict. Where a configuration outside the candidates would have met the spec's criteria and a candidate did not, or the reverse, the report says so plainly and draws no verdict from it.
- [ ] The limits section states: one model, simulation, statistic, constraint, iteration and machine; that candidate configurations were judged on one holdout; that the ranges, the output rule and the removal threshold were set after an exploratory probe; that the noise model (Poisson in `log10 phi`, scale 1) was declared from pilot evidence before the main capture and was not fitted, and that at the pilot's size that evidence shows the noise's size and independence and not the model's effect on calibration; that the discrepancy budget is not defended for this model and box and carries no term for the box's own sample variance, so the removal result is conditional on it; that the constraint's h and IMF conventions are the shipped figure's and were not re-derived; that the verdict validates no gate for mechanical use; that the model was run at the template's `SubSteps`, a setting at which it is not converged in time resolution, so the acceptable region belongs to the model as run; that no second model was run, so the package's independence from the model rests on its contract and a dry run of run-file generation on a second template; and every floored value and unvalidated output by name. Each limit is stated in words. Where a limit rests on a measurement that is not committed in this repository (the time-resolution scan and the exploratory probe, both made before the package existed), the report says so and quotes no number for it; the `SubSteps` setting itself is cited from the Mimic template by path and commit.
- [ ] The README says that a committed `manifest.sha256` lists the capture's per-run files under `runs/`, which are not committed, so the manifest is verified in `output/<capture>` and `analyse` ignores entries for files it does not read.
- [ ] The report and the README say what they answer in their own words and cite no document under Mimic's `docs/dev/`; references to Mimic are to its committed code, by path and commit.
- [ ] A verdict of fail is reported as plainly as a pass, with the criterion that failed and its measured value; nothing in the spec, the code or the evidence is changed to alter a result.
- [ ] After review, a correction edits the report or the README only, from the files already captured; a correction that would need a new capture or a new analysis stops the slice for the PM.
- Inputs: the Mimic checkout at `/Users/dcroton/Local/git-repos/mimic`, built by run preparation, with its binary and the files the run reads unchanged since the pilot (read and execute only); the interpreter `.venv/bin/python`; the frozen spec; `evidence/pilot/` and `reports/PILOT-RECORD.md`; the stages from Slices 1 to 7.
- Inputs, where the figures of the noise-model and emulator-family sections are. All are committed under `evidence/analyses/`, whose `README.md` says what each analysis is; all were made before the main capture existed, on the pilot or on synthetic draws, and the report says so and quotes each figure from its file:
  - `2026-11-10-pilot-noise-model/s01.out`: within a run, the mean square of a bin's `log10` count about the cubic through its four neighbouring bins is 0.98, 1.03, 1.00, 1.02 and 1.17 times the Poisson variance in the count bands 5 to 30, 30 to 100, 100 to 300, 300 to 1,000 and above 1,000. These are ratios of variance (the file's `c_hat`), 0.99 to 1.08 as ratios of standard deviation, and the report labels them so. The same file gives how much adding the Poisson variance would increase the gate's denominator at the default run and the gate budget: at most 2.41% in a core bin. It is an increase of the denominator, not a share of it.
  - `2026-11-10-pilot-noise-model/s05.out`: a moment estimate of the noise scale of 0.89 from the Gaussian process's leave-one-out residuals; a correlation of the scatter between Latin-hypercube runs of at most +0.08 in any band of distance; and the gain in the Gaussian process's log marginal likelihood, summed over the 16 outputs, when the scale is fitted and not declared: 32.7 at scale 1 and 46.9 at 4, the largest tried.
  - `2026-11-10-pilot-noise-model/s03.out`: a correlation of +0.09 between nearest neighbours, and a cross-validated error at 80 training points of 2.4 to 4.8 times the Poisson level for the Gaussian process.
  - `2026-11-10-pilot-noise-model/s04.out` and `s07.out`: on the pilot's folds, outputs validated of 16, averaged over ten fold assignments: Gaussian process 11.6 as built, 11.0 with the noise model, 14.0 with noise at prediction only; Bayes linear 5.4 as built, 6.5 with noise at prediction only, and 5.4 with the declared noise (`s07.out`, line b). A leave-one-out residual variance has a median ratio of 1.00 to the contracted estimate (`s07.out`, line a).
  - `2026-11-10-pilot-noise-model/s06.out`: on a synthetic control at 400 training and 200 test points, with root-mean-square standardised errors for the count bands below 30, 30 to 100, 100 to 300, and 300 and above: Gaussian process as built 14, 16 and 15 of 16 outputs validated in three replicates, with 2.32, 1.58, 1.19 and 0.88, and with the noise model 16, 16 and 16, with 1.01, 1.05, 0.98 and 0.97; Bayes linear as built 14, 16 and 14, with 2.16, 1.51, 1.17 and 1.01, and with the declared noise 13, 14 and 15, with 1.03, 1.17, 1.11 and 1.23.
  - A label the report carries for `s04.out`, `s06.out` and `s07.out`: their fits with the noise model are prototypes made before the package had the term. The Gaussian process's has the package's form. The Bayes linear one (`BLknown(c=1)`) has the package's covariance, but takes five updates of the residual variance with no convergence check and chooses its length and nugget by a score that rescales the declared noise, so its figures support the measurement and are not the package's estimator.
  - `2026-11-11-control-draw-sets/control_draw_sets.out`: on Slice 4's known-good control, draws meeting every criterion, of 30 in each of the sets 1000 to 1029, 2000 to 2029 and 3000 to 3029, are 23, 25 and 21 for the Bayes linear emulator with its selected terms (69 of 90), 26, 29 and 29 with all 84 terms and no selection (84 of 90), and 30, 29 and 30 for the Gaussian process (89 of 90). With all 84 terms the inert input is active on all 30 draws of every set. The file lists each failing draw with its fraction of standardised errors within 2 and its root-mean-square standardised error: 0.800 to 0.895 and 1.20 to 1.49 for the Bayes linear emulator with selected terms.
  - `2026-11-10-bl-calibration-review/e0_baseline.out`: each of draws 1000 to 1029 for the Bayes linear emulator as built, with a root-mean-square error of 0.009 to 0.018 of the function's standard deviation, so the failures are of the variance and not of the mean.
  - `2026-11-10-bl-calibration-review/e1_variants.out` and `e4_more.out`: on draws 1000 to 1029 of Slice 4's control the Bayes linear emulator's mean root-mean-square standardised error is 1.14 on the test points and 1.15 by training leave-one-out, an over-confidence of about 15% in standard deviation; draws passing, of 30, are 23 as built, 30 with a residual variance from leave-one-out residuals, 28 with one correlation length per input, 26 with all 84 terms (the inert input active on all 30), and 29 and 30 with the length grid cut off at 0.5 and at 0.3.
  - `2026-11-10-bl-calibration-review/e3_sim.out`: on 120 Gaussian-process truths at those designs (60 with the anisotropic kernel fitted to control draw 1000 and 60 isotropic), an oracle predicting with the true kernel and noise met the calibration criteria on all 120, and the Bayes linear emulator as built on 60 of 60 isotropic and 56 of 60 anisotropic ones. The report describes it as that, a bound on how often the criteria fail a calibrated emulator and a pointer at the single correlation length; it is not Slice 4's control with an exact variance.
  - `2026-11-10-bl-calibration-review/e5_pilot.out` and `e6_hetero.out`: the pilot's fixed five folds, outputs validated and calibration by count band for the emulators as built.
  - `2026-11-11-count-control-probe/chosen-map-draws-7000-7029.out`: the count control's figures on its five asserted draws and on 30; `2026-11-11-count-control-probe/round2/` holds the check of the selection score the package uses and the probe of the diagnostics stage's controls.
- Behaviour that must not change: every file outside this slice's surface; the Mimic checkout, which the slice reads and never writes (`git -C /Users/dcroton/Local/git-repos/mimic status --short -- models/sage16/input/sage16_mini-millennium.yaml simulations/mini-millennium plot/mimic-plot` prints nothing).

### Authorized Surface
- Files allowed to change:
  - `evidence/main/`
  - `reports/SPIKE-REPORT.md` (new file)
  - `README.md`
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No code or spec change, no second capture, no second analysis run with other settings, no result chosen after seeing it. The docstrings' record of the run-1 control figures stays as it is.
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
- Lint (differential, via the `lint` skill): required. The report and the README are Markdown, on which the lint skill runs markdownlint and codespell; `pyproject.toml` configures only the Python tools.
- Manual checks: an independent read-only trace, by a Reviewer, of every figure in the report to its committed measurement file: a file under `evidence/`, or `reports/PILOT-RECORD.md`.

### Rollback Path
- Revert the slice's commit and move `output/main` aside; the code and the frozen spec are untouched.

---

## After the Run

Owner's steps in Mimic, outside the plan's slices:

1. Read `reports/SPIKE-REPORT.md` and decide: promote the brief to an instrument plan, or close it, as its *First Work* section says.
2. Record the verdict, the measured criteria and a pointer to the emulator repository's commit in [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md) and in the pathway's step 6 row and changelog; move this plan to `archive/dev-plans/`, replacing the links to it in the brief's status line and in the pathway's step 6 row and documents table. The report's two comparisons answer the brief's Open Questions 3 and 4, and its observations on the budget and on stored state bear on Open Questions 1 and 6. The analyses revision 7 rests on are committed in the emulator repository under `evidence/analyses/`; the copy under `archive/emulator-spike-analyses/` holds the binary intermediates where an analysis made any, and is not a full mirror.
3. If promoted, the instrument's plan starts from the report's last section and owes a run on a second model package, the brief's gate items 2 to 5, the items under [Forward Use](#forward-use-campaigns-and-the-model-builder) marked not met, a decision on Decision 2's and Decision 4's alternatives, and the questions Decisions 15 and 16 leave to it: whether the noise scale stays declared at 1 for other statistics and boxes, and whether the Bayes linear emulator gets one correlation length per input or a residual variance from leave-one-out residuals.

---

## Next Chat Prompts

### Mode A — Human-managed plan

```md
Plan file: /Users/dcroton/Local/git-repos/mimic/docs/dev/MIMIC-EMULATOR-IMPLEMENTATION-PLAN.md
Scope: Slice 5
Supervision: checkpointed — I am present: ask me before coding an approval-flagged slice and before each commit
Reviewers (optional): drift auditor opencode opencode-go/deepseek-v4.1-flash max; code reviewers claude claude-opus-5-5 high; codex gpt-6.1-sol high; opencode opencode-go/qwen3.8-flash xhigh

Read the full plan file first. If a slice or batch receipt in scope is incomplete or the plan state is unclear, stop and tell me before coding.

The work happens in /Users/dcroton/Local/git-repos/mimic-emulator, not in Mimic. Slices 1 to 4 are complete on the branch spike/calibrated-variance, whose head is 1acd279 (Slice 4's commit 56d7115, then five commits that add evidence/analyses/ and exclude evidence/ from the formatters and linter): work on that branch, and tell me if its head is anything else or its worktree is not clean. The plan's Run Preparation was completed before Slice 1 and is not repeated: do not rebuild ./mimic or recreate the environment. Make the checks the plan's Execution Structure lists under "Before run 2" before anything else.

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
Drift auditor: opencode opencode-go/deepseek-v4.1-flash max
Code reviewers: claude claude-opus-5-5 high; codex gpt-6.1-sol high; opencode opencode-go/qwen3.8-flash xhigh
Sandbox policy: /Users/dcroton/Local/git-repos/mimic/archive/sandbox-policies/MIMIC-EMULATOR-SANDBOX.json
Run tag (optional): emulator-spike
Use the project-manager skill. You are the PM: the accountable supervisor of this run. You never write slice code.

This is run 2 of the plan. Run 1 (20261110T090410Z-ea3675) completed Slices 1 to 4 under the plan's revision 6 and was ended; run 2 executes Slices 5 to 8 under revision 7, on the same branch, with Slices 1 to 4 attested.

Handoff: HANDOFF.md in the repo root, git-ignored (add it to .git/info/exclude if it is not; a campaign child's is already excluded), is the sole entry point for a restart and for my post-run review. It exists and holds run 1's record: keep that record, and add run 2's after init. Update it after every slice decision and before any stop with: run identity (never the token), seats, branch, commit authorisation, review protocol, slice log, plan decisions, residuals and follow-ups, and PM lessons.

Starting the run: `init` takes full model ids, efforts where the harness supports them, the run tag, `--sandbox <path>` with the policy above (the same file as run 1, SHA-256 5d9c46437f258e0e44561edaa786f227f2a6763d74eba7faecd21340c9c21153), `--branch spike/calibrated-variance` (the branch exists, at 1acd279: do not create one) and `--attest "Slice 1,Slice 2,Slice 3,Slice 4"`; set `PM_TMUX_SOCKET` exactly as the `export PM_TMUX_SOCKET=…` line init prints, for every later command of the run.

Human present (the default):
- Read HANDOFF.md fully. If it names an unfinished run 2 of this plan, resume that run (I will supply the token). Otherwise prepare, then confirm with me once:
  - Read the whole plan, run check-plan with repo context, and check that the repo still matches what the plan assumes. Init freezes the plan, so raise any drift or warning now.
  - The plan is committed in the Mimic repository, not in the run's repository, which meets the plan-committed requirement. The plan's Run Preparation was completed before run 1 and is not repeated: do not rebuild ./mimic or recreate the environment. Make the checks the plan's Execution Structure lists under "Before run 2", and raise any that fails.
  - Require a clean worktree in the run's repository, on the existing branch spike/calibrated-variance at 1acd279.
  - Confirm: plan read, check-plan result, seats, branch, the attested slices, first slice (Slice 5), any approval-flagged slices (each stops the run until I approve it), my standing authorisation for Developers to commit each slice, and, when the Developer's harness asks for directory trust per worktree, my permission to add the repo's trust entry yourself (project-manager's references/sandbox.md *Troubleshooting*).
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
