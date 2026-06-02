# Mimic — Theoretical Methods Landscape & Opportunity Review

**Report type:** Comparison / opportunity assessment
**Date:** 2026-06-02
**Author:** Claude (Opus 4.8), reviewed against `docs/VISION.md`

---

## Objective

Survey and classify the theoretical methods used to model galaxy formation and
evolution, and galaxy/dark-matter properties; for each, summarise the method,
the current gaps in the field, the opportunity, and a high-level sketch of how
Mimic could be applied. Define a rubric grounded in Mimic's *actual* strengths,
score every method against it, and produce a ranked list of where Mimic is most
profitably applied — including creative but physically defensible hybrid and
scale-mixing opportunities.

## Scope

**Included**

- Physics-based, empirical, analytic, data-driven (ML), and statistical-inference
  methods for the galaxy–halo connection and galaxy evolution.
- Cross-cutting *applications* (survey mocks, inference, reionization sources)
  and *hybrid* opportunities (grey-box physics+ML, multi-fidelity scale mixing).
- An explicit fit rubric and weighted scorecard.

**Excluded**

- Re-derivation of the astrophysics itself (this is a methods/architecture review,
  not a physics review).
- Detailed code design for any single opportunity (those become follow-on `plan`
  reports).
- Cosmological-parameter inference where galaxies are nuisance terms only
  (touched on, not surveyed).

## Context

Mimic is a **physics-agnostic core with runtime-configurable physics modules**
that operates on dark-matter **merger trees / FoF workspaces** (`docs/VISION.md`).
It is descended from SAGE and already ships two contrasting model packages — a
full physics-based SAM (`models/sage/`) and an empirical stellar-to-halo-mass
package (`models/sham/`) — which demonstrates the framework already spans the
physics-based ↔ empirical axis.

Two architectural facts dominate this review:

- **Strength:** physics is decomposed into swappable, metadata-declared modules
  executed in a phased pipeline (`pre_timestep → phase_1 → phase_2 →
  post_timestep`) with `full_halo`, `by_galaxy`, and `per_event` dispatch modes,
  selectable at runtime without recompiling. Output carries full provenance.
- **Constraint:** Mimic **processes one FoF workspace at a time** and keeps
  per-tree memory bounded (VISION principles 4 and 5). It therefore has **no
  native volume-wide / snapshot-wide ranking or statistical-calibration stage**.
  This is stated explicitly in `models/sham/README.md`: the shipped "SHAM" is a
  per-branch *proxy*, not a true global abundance match. This single fact is the
  main discriminator between methods that fit Mimic today and methods that would
  require a deliberate architectural extension.

## Comparison Criteria (the rubric)

Derived from Mimic's strengths and constraints. Each method is scored 1–5 per
criterion; criteria are weighted by how central they are to what Mimic uniquely
offers.

| # | Criterion | What it rewards | Weight |
|---|-----------|-----------------|:------:|
| R1 | **Merger-tree native** | Method's fundamental input is DM halo assembly history / merger trees | 3 |
| R2 | **Modular decomposition** | Physics expresses cleanly as discrete, swappable pipeline stages | 2 |
| R3 | **Ablation / model-swap value** | Science needs controlled comparison, disabling physics, A/B prescriptions | 2 |
| R4 | **Throughput-bound** | Science needs many fast evaluations (sweeps, MCMC, SBI, training sets) | 2 |
| R5 | **Per-object locality** | Works per-FoF/per-tree **without** volume-wide global ranking | 3 |
| R6 | **Property/metadata extensibility** | Needs new tracked quantities with auto-generated schema/provenance | 1 |
| R7 | **Reproducibility/provenance value** | Community needs auditable, reproducible, self-describing runs | 1 |

Weighted maximum = 70. "Fit %" = weighted score / 70.

**Rubric limitation (declared up front):** the rubric measures *fit to Mimic's
current paradigm*. It does **not** capture a method's intrinsic scientific value,
nor does it penalise a paradigm mismatch that is invisible to these axes — most
notably **automatic differentiability** (gradient-based forward models), which
scores well on R1–R5 yet is fundamentally a different computational paradigm from
Mimic's C pipeline. Where the rubric and judgment disagree, judgment is stated
explicitly and the ranking is overridden.

## Method

- Read `docs/VISION.md` and the project `AGENTS.md`/`CLAUDE.md` to extract
  architectural strengths and constraints.
- Inspected `models/` (`sage`, `sham`), `models/sage/modules/` (18 physics
  modules), and `models/sham/README.md` to confirm the framework's real span and
  the per-FoF constraint.
- Web-searched current (2021–2026) literature on: SAM↔ML emulation, GNNs on
  merger trees (Mangrove), differentiable forward models (diffsky/Diffstar/
  DiffstarPop/sapphire), simulation-based inference for the galaxy–halo
  connection, and survey mock/lightcone pipelines, to verify currency.
- Scored each method against the rubric; cross-checked the ranking against all
  seven VISION principles (see *Risks / Unknowns*).

## Evidence

- `docs/VISION.md` — architectural principles (per-FoF processing, runtime
  modularity, metadata-as-truth, bounded memory, reproducible provenance).
- `models/sham/README.md` — explicit statement that true SHAM needs global
  volume/snapshot-wide ranking that Mimic's one-FoF-at-a-time path cannot
  currently provide.
- `models/sage/modules/` — existing decomposition of SAM physics into ~18
  swappable modules (cooling, infall, star formation, supernova/starburst
  feedback, quasar & radio AGN modes, reincorporation, disk instability,
  stripping, reionization, mergers), evidence that real galaxy physics already
  maps onto the module model.
- Literature (selected, see per-method references): canonical SAM, SHAM, HOD,
  empirical-forward, equilibrium, hydro, and ML/SBI papers, plus 2022–2026
  results (Mangrove; diffsky ecosystem; GALFORM deep-learning emulator; hybrid
  SBI for surveys).

## Options

Each method below follows: **Summary → Field gaps → Opportunity → Mimic usage →
Key references → Fit**. They are presented in ranked order (see *Decision* for
the consolidated table).

---

### 1. Physics-based semi-analytic models (SAMs) — Fit 100%

- **Summary.** Solve coupled prescriptions for gas cooling, star formation,
  stellar/AGN feedback, chemical enrichment, mergers, and morphological
  transformation along DM merger trees. The dominant fast physical model of the
  galaxy population. *Mimic is itself a SAM engine.*
- **Field gaps.** Prescription degeneracy (different physics, similar
  luminosity functions); calibration is high-dimensional and partly subjective;
  cross-code comparisons are confounded by differing trees, units, and
  bookkeeping; AGN feedback and quenching remain weakly constrained; unit/
  bookkeeping bugs are easy to hide (cf. the quasar-wind unit issue logged in the
  Mimic↔SAGE parity analysis).
- **Opportunity.** A *physics-agnostic* SAM with metadata-generated state and
  reproducible provenance directly attacks the comparison-confounding and
  hidden-assumption problems that plague the field.
- **Mimic usage.** This is home turf: author/swap physics modules, calibrate,
  run production trees, emit self-describing output. The differentiator vs.
  GALFORM/L-Galaxies/Shark is engineering discipline (one model set, generated
  schema, provenance) rather than new physics.
- **References.** White & Frenk 1991; Cole et al. 2000; Croton et al. 2006, 2016
  (SAGE); De Lucia & Blaizot 2007; Henriques et al. 2015; Lacey et al. 2016;
  Lagos et al. 2018 (Shark); Benson 2012 (Galacticus); Somerville & Davé 2015
  (review).

### 2. Controlled physics ablation / numerical experiments — Fit 96%

- **Summary.** Not a model but a *method of inquiry*: hold trees and calibration
  fixed and toggle/replace one prescription at a time to attribute observable
  features to specific physics. The field does this rarely and unsystematically
  because most codes require recompilation and lack provenance.
- **Field gaps.** "Which physics produces this feature?" is usually answered
  anecdotally. Runs are not reproducible enough to trust differential
  conclusions; the controlled variable is rarely *only* the physics under test.
- **Opportunity.** Mimic's runtime module selection + reproducible provenance
  makes physics ablation a *first-class, auditable experiment* — arguably its
  single most distinctive scientific capability.
- **Mimic usage.** Same executable, YAML-only changes: disable supernova
  feedback, swap AGN mode, run halo-tracking-only baseline, diff the outputs with
  guaranteed-identical everything-else. Each run self-documents its active
  pipeline.
- **References.** Methodologically novel framing; closest analogues are SAM
  sensitivity studies, e.g. Croton et al. 2006 (AGN on/off), Henriques et al.
  2015 (reincorporation variants), and Gabrielpillai et al. 2022.

### 3. Emulator training-set factory + SBI forward simulator — Fit 87%

- **Summary.** Two coupled uses of Mimic as a *generator*: (a) produce large,
  labeled `(merger-tree → galaxy)` datasets to train surrogate emulators; (b)
  serve as the forward simulator inside simulation-based / likelihood-free
  inference (SBI) of galaxy-formation parameters.
- **Field gaps.** Emulators and SBI are bottlenecked by the cost and
  reproducibility of the forward model; hydro is too slow to generate the
  104–106 samples SBI wants; training/inference provenance is often poor,
  undermining reproducibility of posteriors.
- **Opportunity.** A fast, scriptable, provenance-emitting forward model is
  exactly the missing piece. Mimic can be the cheap, auditable simulator that
  makes amortised inference over *physical* (not just empirical) parameters
  practical.
- **Mimic usage.** Parameter sweeps via YAML over Latin-hypercube designs →
  emit HDF5 with parameters embedded → train GP/NN emulator or feed an SBI engine
  (e.g. neural posterior estimation). A virtuous loop with method 8: the trained
  surrogate can be re-imported as a fast Mimic module.
- **References.** Elliott et al. 2021 (deep-learning emulator of GALFORM);
  Cranmer, Brehmer & Louppe 2020 (SBI review); Alsing et al. 2019; Hahn et al.
  2023 (SimBIG); Modi et al. 2025 (hybrid SBI for galaxy surveys, arXiv:2505.13591).

### 4. Hybrid physics + ML "grey-box" modules — Fit 87%

- **Summary.** Replace an *individual* uncertain or expensive prescription (e.g.
  the cooling→SFR map, quenching, or a sub-grid relation calibrated from hydro)
  with a learned function, while leaving the rest of the pipeline analytic. The
  emerging frontier (hybrid equilibrium-model+ML emulators; `sapphire`).
- **Field gaps.** Pure-ML galaxy models are black boxes with poor extrapolation
  and no physical interpretability; pure-analytic models carry rigid functional
  assumptions. Few frameworks let you mix *per-prescription*.
- **Opportunity.** Mimic's per-module interface is the natural substrate for
  grey-box physics: one learned module among many analytic ones, with the same
  declared properties, units, and provenance. This is a genuinely novel and
  realistic capability, **not** overhype — the module boundary already exists.
- **Mimic usage.** Implement a learned prescription as a module inside a model
  package (it must reconcile properties/units/dependencies per the Model-Set
  Boundary). Ablate it against its analytic sibling using method 2.
- **References.** Agarwal, Davé & Bassett 2018; de Andres et al. 2023 (hybrid
  equilibrium+ML); McGibbon & Khochfar 2023; `sapphire` (Pearl et al. 2026,
  arXiv:2604.06318); Kamdar et al. 2016.

### 5. Analytic equilibrium / gas-regulator ("bathtub") models — Fit 86%

- **Summary.** Reduce galaxy evolution to a low-dimensional ODE: SFR set by the
  balance of accretion, outflow, and gas reservoir along the halo accretion
  history. Captures the star-forming main sequence and mass–metallicity relation
  with few parameters.
- **Field gaps.** Equilibrium assumptions break for bursty, low-mass, and
  high-z galaxies; they are usually run as standalone toy models divorced from a
  full tree-based pipeline and from a consistent merger/satellite treatment.
- **Opportunity.** A bathtub model is *literally a per-halo ODE driven by the
  mass-accretion history* — an excellent lightweight Mimic module and an ideal
  fast baseline/null-model for ablation and SBI.
- **Mimic usage.** Implement as a single `by_galaxy` module consuming the
  tree-derived accretion rate; use as the cheap arm in multi-fidelity studies and
  as the SBI null model against the full SAM.
- **References.** Bouché et al. 2010; Davé, Finlator & Oppenheimer 2012; Lilly
  et al. 2013 (gas regulator); Dekel & Mandelker 2014; Peng et al. 2010.

### 6. Empirical assembly-history forward models (UniverseMachine / EMERGE) — Fit 84%

- **Summary.** Parametrise galaxy SFR directly as a function of halo properties
  *and their history* (e.g. SFR(Vmax, dVmax/dt, z)), then constrain the
  parameters against observed statistics. EMERGE and UniverseMachine apply the
  SFR per halo along its track.
- **Field gaps.** The *application* is per-history (fits Mimic), but the
  *calibration* is a global fit to volume-level statistics (clustering, SMF,
  quenched fractions) — exactly the volume-wide reduction Mimic lacks natively.
  Empirical relations also encode rather than explain physics.
- **Opportunity.** Mimic is an excellent *executor* of these per-history
  relations as modules, and an excellent fast simulator for their global
  calibration loop (via method 3). Bridges empirical and physical pipelines in
  one engine.
- **Mimic usage.** Implement the SFR(history) relation as a module; outsource the
  global-statistics calibration to an external SBI/MCMC driver that calls Mimic.
- **References.** Behroozi et al. 2013, 2019 (UniverseMachine); Moster, Naab &
  White 2013, 2018 (EMERGE); Wechsler & Tinker 2018 (review).

### 7. Survey mock & lightcone production (DESI / LSST / Euclid) — Fit 79%

- **Summary.** Populate large N-body volumes with galaxies and assemble
  lightcones with realistic colours, selections, and clustering for survey
  forecasting, covariance, and systematics.
- **Field gaps.** Mock pipelines are often bespoke, weakly documented, and hard
  to reproduce; provenance of which physics/parameters produced a given mock is
  frequently lost — a growing concern for Stage-IV survey reproducibility.
- **Opportunity.** Mimic's self-describing output (enabled modules, parameters,
  redshift map, version) is tailor-made for *auditable* survey mocks. Lightcone
  assembly (snapshot stitching across the volume) is the part that touches
  Mimic's per-FoF/global boundary and would need a post-processing stage.
- **Mimic usage.** Run the SAM on survey-scale trees → emit provenance-rich
  catalogues → external lightcone-assembly/selection layer. The per-galaxy
  physics stays inside Mimic; the volume stitching stays outside it.
- **References.** Merson et al. 2013 (SAM lightcones); Smith et al. 2017
  (Millennium-XXL/DESI BGS); Korytov et al. 2019 (cosmoDC2/LSST); DESI mock
  challenge papers (2022).

### 8. GNN / ML galaxy–halo surrogate on merger trees (Mangrove) — Fit 79% *(synergy, not native method)*

- **Summary.** Train a graph neural network to regress baryonic properties
  directly from the merger-tree graph — 104–109× faster than SAMs/hydro, often
  more accurate than other ML mappings.
- **Field gaps.** GNN surrogates need large, *consistent* training sets and
  inherit the biases of whatever produced the labels; they are black boxes and do
  not give controlled physical insight.
- **Opportunity.** Strong two-way synergy rather than competition: **Mimic
  generates the labeled training trees** (method 3), and the trained surrogate can
  be **wrapped as a fast Mimic emulator-module** for regimes where full physics is
  overkill (feeds methods 4 and the multi-fidelity idea below).
- **Mimic usage.** Mimic as data factory and as the host for the resulting
  surrogate module; ablate surrogate-vs-physics with method 2.
- **References.** Jespersen et al. 2022 (Mangrove, ApJ 941 7); Villaescusa-
  Navarro et al. 2021 (CAMELS); Moster et al. 2021 (GalaxyNet); de Santi et al.
  2022.

### 9. Semi-numerical reionization — source modeling — Fit 69%

- **Summary.** Model the ionizing-photon budget from galaxies/halos and its
  effect on the IGM (21cm, ionization history). Mimic already has a
  `sage_reionization` module on the *consumer* side.
- **Field gaps.** The *source* side (ionizing emissivity per halo across history)
  is local and fits Mimic; the *field* side (the radiation field is a global,
  volume-coupled quantity) does not — it needs cross-FoF/volume coupling Mimic
  does not provide.
- **Opportunity.** Mimic can be a high-fidelity, history-resolved *source model*
  feeding an external semi-numerical IGM solver (e.g. 21cmFAST-style), improving
  on the crude source prescriptions those codes typically use.
- **Mimic usage.** Emit per-halo ionizing emissivity histories with provenance →
  external volume-coupled radiative solver. Do **not** attempt the global
  radiation field inside Mimic without a deliberate volume-coupling stage.
- **References.** Mesinger et al. 2011 (21cmFAST); Mutch et al. 2016 (Meraxes —
  SAM coupled to reionization, the closest precedent); Seiler et al. 2019.

### 10. Intrinsic alignments / galaxy shape & orientation — Fit 66% *(niche)*

- **Summary.** Assign galaxy shapes/orientations from halo spin and shape to
  model intrinsic-alignment contamination of weak-lensing surveys.
- **Field gaps.** IA models are largely empirical/statistical; physically
  motivated, history-aware assignments tied to tracked halo spin are
  underexplored.
- **Opportunity.** Mimic already tracks spin; emitting orientation/shape proxies
  is a low-cost property-extensibility win for a real Stage-IV systematics need.
- **Mimic usage.** Add spin/shape-derived orientation properties via metadata;
  feed an external IA/lensing pipeline.
- **References.** Joachimi et al. 2015 (review); Tenneti et al. 2016; Samuroff
  et al. 2021.

### 11. Decorated HOD / assembly-bias models — Fit 64%

- **Summary.** Extend the HOD by conditioning galaxy occupation on a secondary
  halo property (concentration, formation time) to capture assembly bias.
- **Field gaps.** The occupation statistics are a *global, volume-calibrated*
  statistical model — Mimic's missing capability. Mimic *can* supply the
  secondary properties (it tracks formation history/concentration from trees),
  but not the statistical calibration.
- **Opportunity.** Mimic as the *provider of physically-grounded secondary
  properties* for external decorated-HOD frameworks, replacing ad hoc proxies.
- **Mimic usage.** Emit tracked secondary halo properties; calibration/occupation
  stays in an external halo-model toolkit (e.g. halotools).
- **References.** Hearin et al. 2016 (decorated HOD); Wechsler et al. 2006; Gao,
  Springel & White 2005; Hadzhiyska et al. 2020 (AbacusHOD).

---

### Complement / contrast tier — strong methods that Mimic should *interface with*, not *implement*

### 12. Differentiable / autodiff forward models (diffsky / Diffstar / sapphire) — rubric 79%, **judgment: complement**

- **Summary.** JAX/GPU forward models of star-formation histories and the
  galaxy–halo connection with end-to-end gradients, enabling gradient-based
  optimisation and inference over physical parameters.
- **Why not implement in Mimic.** Their defining feature is *automatic
  differentiability* — a paradigm Mimic's C pipeline does not provide. The rubric
  scores them well (they are tree/history native and modular) but is blind to this
  mismatch. Re-engineering Mimic for autodiff would be a rewrite, not a module.
- **Opportunity (realistic).** Treat as a **complementary inference layer and
  cross-check**: Mimic provides the interpretable, ablatable physical model and
  the non-differentiable forward simulator for SBI; the diffsky ecosystem provides
  gradient-based calibration. Cross-validating the two is high-value, low-overhype.
- **References.** Hearin et al. 2021 (Diffstar/diffsky); Alarcon et al. 2025
  (DiffstarPop, arXiv:2510.27604); Pearl et al. 2026 (sapphire); Hearin et al.
  2023 (DSPS).

### 13. True subhalo abundance matching (SHAM) — Fit 50% *(needs architectural extension)*

- **Summary.** Assign galaxy properties by a global monotonic match between
  cumulative galaxy and (sub)halo abundances across a whole snapshot/volume.
- **Why limited today.** Requires the exact volume-wide ranking stage Mimic lacks
  (`models/sham/README.md`); the shipped package is an honest per-branch *proxy*.
- **Opportunity.** A deliberate **optional volume-wide reduction/ranking phase**
  (a post-traversal stage that gathers a peak-property proxy per branch, ranks
  globally, assigns, then writes) would unlock true SHAM *and* the global-
  calibration legs of methods 6, 7, and 11. This is the single highest-leverage
  architectural extension — but it genuinely tensions VISION principles 4–5
  (one-FoF-at-a-time, bounded per-tree memory) and must be designed as an opt-in
  stage, not a default.
- **References.** Vale & Ostriker 2004; Conroy, Wechsler & Kravtsov 2006;
  Reddick et al. 2013; Behroozi, Conroy & Wechsler 2010.

### 14. True halo occupation distribution (HOD) — Fit 50%

- **Summary.** Populate halos statistically from P(N|M_halo) calibrated to
  clustering; the workhorse of galaxy-clustering cosmology.
- **Why limited.** Global statistical calibration (same constraint as SHAM) and a
  statistical, non-dynamical character that under-uses Mimic's history tracking.
- **Opportunity.** Same optional global-stage extension as method 13; otherwise
  Mimic best serves HOD work as a *physical prior generator* for occupation
  shapes.
- **References.** Berlind & Weinberg 2002; Zheng et al. 2005, 2007; Cooray &
  Sheth 2002 (halo model review).

### 15. Full hydrodynamical simulations — Fit 33% *(not an implementation target)*

- **Summary.** Solve gravity + hydrodynamics + sub-grid feedback on a resolved
  mesh/particle set (Illustris/TNG, EAGLE, SIMBA). The most physically complete
  but most expensive approach.
- **Why not a fit.** Mimic is not a hydro solver and should not become one;
  resolved gas dynamics and radiative transfer are outside its model.
- **Opportunity (complementary).** Hydro is Mimic's **calibration target,
  module-trainer, and multi-fidelity anchor**: train grey-box modules (method 4)
  and surrogates (method 8) on hydro; use a small hydro box to anchor a large
  cheap Mimic volume (see hybrid idea below).
- **References.** Vogelsberger et al. 2014 (Illustris); Schaye et al. 2015
  (EAGLE); Pillepich et al. 2018 (TNG); Davé et al. 2019 (SIMBA); Vogelsberger
  et al. 2020 (review).

---

### Creative cross-scale / hybrid opportunities (physically defensible)

These exploit Mimic's modularity and processing modes rather than adding new
single methods. Included only because they are realistic, not aspirational.

- **Resolution-adaptive physics within one run.** Dispatch *full SAM physics in
  well-resolved/massive FoF systems* and a *cheap empirical or surrogate
  assignment in poorly-resolved/low-mass halos*, selected per-FoF. This is a
  natural use of the existing dispatch modes and directly addresses the
  resolution-dependence problem that biases SAM low-mass predictions. **Defensible
  now**; the selection logic is local.
- **Multi-fidelity / scale-bridging.** Calibrate a grey-box or surrogate module
  against a *small hydro box*, then run the cheap module across a *Gpc N-body
  volume* in Mimic — Mimic as the fast, large-volume arm of a two-fidelity
  pipeline. **Defensible**; matches the emulator/SBI tooling.
- **Surrogate-in-the-loop acceleration.** Use a Mangrove-style surrogate (method
  8) as a Mimic module to skip full physics where a feature has converged, falling
  back to physics where it has not. **Defensible** as a module; needs a confidence
  gate.
- **Honest non-starters (stated to avoid overhype).** A *global radiation field*
  for reionization, *true global SHAM/HOD calibration*, and *autodiff gradients*
  are **not** free-lunch modules — each requires a deliberate architectural stage
  (volume coupling, ranking phase, or a different compute paradigm) that tensions
  VISION principles 4–5. They are opportunities for *scoped extensions or external
  interfaces*, not drop-in modules.

## Architectural Pivot Opportunity: Snapshot-Synchronized Processing

This is the single most consequential extension surfaced by the review, so it is
called out separately. It is the general form of the "opt-in global stage"
referenced under ranks 7/13/14.

### The two processing orders

- **Current — tree/forest-ordered (depth-first).** Mimic walks each forest and
  processes one FoF workspace at a time, advancing physics along branches. Memory
  is bounded per-tree (VISION principle 5); forests are embarrassingly parallel
  under MPI. Excellent for *per-history* physics; structurally unable to see other
  halos at the same cosmic time.
- **Proposed (additive) — snapshot-synchronized (breadth-first in time).** Make
  the **outer loop over snapshots (cosmic time)**: at each snapshot, (a) advance
  every halo's per-history physics from the previous snapshot, then (b) run an
  optional **collective stage** with the *entire halo/galaxy population at that
  redshift in scope*, then (c) write/advance. Step (a) is exactly today's physics;
  step (b) is new and is where global operations become possible.

This is not exotic. **It is how the codes that need global coupling already
work:** L-Galaxies processes snapshot-synchronized, and UniverseMachine and EMERGE
are explicitly timestep/snapshot-synchronized *precisely because* their
constraints (abundance matching, global SFR–statistics calibration) require all
halos at a given time simultaneously. Adopting a snapshot-synchronized mode aligns
Mimic with the established architecture for that whole method class.

### What it unlocks (and which ranks it promotes)

- **True global abundance matching / SHAM (rank 13 → native).** Rank all
  halos/subhalos at a snapshot by a peak proxy, match to a cumulative galaxy
  abundance, assign — the exact step `models/sham/README.md` says is currently
  impossible.
- **True HOD / decorated HOD calibration (ranks 14, 11 → native).** Volume-level
  occupation statistics and secondary-property conditioning become first-class.
- **Synchronous cross-halo coupling — a genuinely new physics class.** All halos
  at time *t* can share global state: a **reionization UV background / radiation
  field** (promotes rank 9 from "source only" to full coupling), **environment-
  dependent quenching and tidal/ram-pressure fields**, mean-density and
  large-scale-structure-aware prescriptions, and **assembly bias by construction**
  rather than by post-hoc decoration.
- **Lightcone assembly (rank 7 → native).** Snapshots are the natural unit of a
  lightcone; a time-ordered core makes shell stitching and on-the-fly lightcone
  output structural rather than bolted-on.
- **Global calibration loops for empirical-forward models (rank 6).** The
  volume-statistic calibration legs of EMERGE/UniverseMachine-style models move
  inside the engine instead of requiring an external driver.

### Costs and tensions (stated honestly)

- **Memory model changes (VISION principle 5).** The bound shifts from
  *O(per-tree)* to *O(halos alive at a snapshot)* — for Gpc volumes this is large
  but tractable, and it is precisely the footprint real snapshot-synchronized SAMs
  accept. Bounded memory is preserved, but the *bound is different and larger*;
  this must be a conscious, documented trade, not a silent regression.
- **Persistent galaxy state between snapshots.** Galaxy state must be carried
  across snapshots and re-associated via descendant pointers, rather than living
  only within a forest traversal. This is standard in L-Galaxies/UniverseMachine
  but is real new bookkeeping (progenitor→descendant inheritance, orphan carry-
  over) that the core must own (VISION principle 4).
- **Parallelism changes.** Embarrassingly-parallel forests give way to
  snapshot-synchronized execution with a barrier and (for the collective stage)
  domain decomposition + communication. This is the largest engineering cost and
  the main risk to throughput.
- **Physics-agnostic core must stay agnostic (principle 1).** The collective stage
  must expose a *generic* interface (a per-snapshot reduction/population hook that
  modules opt into), not hard-coded SHAM/reionization. Otherwise the core absorbs
  physics it must not own.

### Recommended shape: additive dual-mode, not a replacement

Do **not** discard tree-ordered processing. The lower-risk, higher-value design is
a **dual-mode core**: keep tree-local per-history physics as the inner step, and
add a snapshot-synchronized outer loop that fires an **optional collective hook**
once per snapshot. Modules declare whether they need the collective stage (a new
dispatch mode alongside `full_halo`/`by_galaxy`/`per_event`, e.g.
`per_snapshot_collective`). Models that never use it pay nothing and behave as
today; models that opt in unlock the entire global-method class above. This keeps
VISION principle 4 ("one coherent processing model") intact — the collective stage
is a *dispatch mode within the model*, not a separate algorithm — and confines the
new memory/parallel cost to runs that actually request it.

**Net assessment.** This is the highest-leverage architectural move available:
it converts five separate "needs an extension" caveats (ranks 7, 9, 11, 13, 14)
into native capabilities and opens a *new* physics class (synchronous environment/
radiation coupling) that no current Mimic mode can express. It is also the most
expensive and the one in genuine tension with principles 4–5, so it warrants a
dedicated design spike and an explicit opt-in, not a default flip. It is promoted
to the top of *Recommended Next Actions*.

## Findings

- **Confirmed:** Mimic already decomposes real SAM physics into ~18 swappable
  modules and already spans physics-based ↔ empirical via two model packages.
- **Confirmed:** The per-FoF/bounded-memory design cleanly supports per-history
  methods (SAM, equilibrium, empirical-forward application, grey-box modules,
  surrogate hosting) and *blocks* native global-ranking/global-statistics methods
  (true SHAM, true HOD, decorated-HOD calibration, lightcone stitching, global
  radiation fields) — a single, consistent discriminator across the ranking.
- **Confirmed (literature currency):** the live frontier is hybrid physics+ML,
  GNN surrogates on trees, differentiable forward models, and SBI — all of which
  position a fast, modular, provenance-emitting tree engine as a high-value
  *component*, not a competitor.
- **Assumption:** that the additive snapshot-synchronized mode is feasible without
  breaking the core model or throughput — plausible (it is the established design
  in L-Galaxies/UniverseMachine/EMERGE) but unverified for Mimic; needs a design
  spike.
- **Confirmed:** the single per-FoF discriminator that gates ranks 7/9/11/13/14 is
  exactly what a snapshot-synchronized outer loop removes — the architecture
  question and the methods-coverage question are the same question.

## Tradeoffs

- **Fit vs. scientific completeness.** The highest-fit methods (SAM, ablation,
  equilibrium) are scientifically *reduced* relative to hydro; the most complete
  method (hydro) is the worst fit. Mimic's niche is the fast, interpretable,
  controllable middle — and as the simulator/data-factory feeding ML and SBE.
- **Locality vs. global statistics.** Honouring VISION principles 4–5 keeps Mimic
  fast and reproducible but excludes global-ranking methods. Relaxing them (an
  opt-in global stage) unlocks SHAM/HOD/lightcones at real architectural cost.
- **Physics vs. ML.** Grey-box modules buy speed/accuracy but cost
  interpretability; Mimic's ablation capability is the mitigation (you can always
  diff surrogate vs. physics).

## Decision

**Ranked application of Mimic to the methods landscape** (rubric %, with judgment
overrides noted):

| Rank | Method / application | Type | Fit | Status |
|:----:|----------------------|------|:---:|--------|
| 1 | Physics-based SAMs | Method | 100% | Native — home turf |
| 2 | Controlled physics ablation experiments | Method (of inquiry) | 96% | Native — signature capability |
| 3 | Emulator training-set factory + SBI forward simulator | Application | 87% | Strong, high-leverage |
| 4 | Hybrid physics+ML grey-box modules | Hybrid | 87% | Strong, novel, realistic |
| 5 | Analytic equilibrium / gas-regulator modules | Method | 86% | Strong, ideal fast baseline |
| 6 | Empirical assembly-history forward models | Method | 84% | Strong (application local; calib external) |
| 7 | Survey mock & lightcone production | Application | 79% | Good (stitching stays external) |
| 8 | GNN/ML galaxy–halo surrogate (Mangrove) | Synergy | 79% | Good — two-way data/module loop |
| 9 | Semi-numerical reionization (source side) | Application | 69% | Partial — source fits, field is global |
| 10 | Intrinsic alignments / shape modeling | Application | 66% | Niche but real |
| 11 | Decorated HOD / assembly-bias (property provider) | Method | 64% | Partial — Mimic supplies inputs only |
| 12 | Differentiable forward models (diffsky/sapphire) | Contrast | 79%→demoted | **Complement, not implement** (autodiff mismatch) |
| 13 | True SHAM | Method | 50% | Needs opt-in global-ranking stage |
| 14 | True HOD | Method | 50% | Needs global statistical stage |
| 15 | Full hydrodynamical simulations | Contrast | 33% | **Complementary anchor, not a target** |

**Primary recommendation:** concentrate Mimic's distinctive value on ranks 1–6 —
the per-history physics, ablation, grey-box, and the *simulator/data-factory* role
for ML and SBI — and treat ranks 12 and 15 as interfaces/complements rather than
build targets. Treat an **additive snapshot-synchronized processing mode** (see
the dedicated section above) as the one architectural investment that, if pursued,
promotes ranks 7, 9, 11, 13, and 14 from "needs extension" to native *and* opens a
new synchronous-coupling physics class — at the cost of a larger memory bound and a
changed parallel model, so it must be opt-in, not default.

## Decision Rationale

- Tied to the rubric: ranks 1–6 score high on the heavily-weighted R1 (tree
  native) and R5 (per-object locality) *and* on R3/R4 (ablation/throughput) — the
  axes where Mimic is genuinely differentiated, not just adequate.
- The cut between "native" and "needs extension" tracks exactly one architectural
  fact (per-FoF vs. global), giving the ranking a single, defensible spine rather
  than ad hoc scoring.
- Judgment overrides (12, 15) are where the rubric is blind to a paradigm mismatch
  (autodiff; hydro) and are flagged as such, satisfying the "no overhype"
  requirement.
- Consistency with VISION: ranks 1–6 honour all seven principles; the global-stage
  recommendation is explicitly marked as in tension with principles 4–5 and as
  opt-in, not default.

## Risks / Unknowns

- **Unverified:** feasibility/cost of an opt-in volume-wide reduction stage
  without violating bounded-memory guarantees (principle 5). Needs a design spike.
- **Unverified:** how much the per-FoF locality actually limits lightcone/HOD use
  in practice vs. a thin external post-processing layer being sufficient.
- **Assumption:** literature currency is adequate from targeted 2021–2026
  searches; a systematic review could surface additional niche fits (e.g. tidal-
  stream/satellite-orbit modeling, which Mimic's merger-clock machinery partly
  supports and which was not scored here).
- **Not checked:** Mimic's actual performance/throughput numbers (no benchmark run
  performed for this report) — the "fast enough for SBI" claim is architectural,
  not measured.
- **Scope risk:** rankings reflect *fit*, not scientific priority for the user's
  own research agenda; the two may differ.

## Recommended Next Actions

1. **Design spike on the additive snapshot-synchronized mode** (the dedicated
   section above). Scope the dual-mode core, the `per_snapshot_collective` dispatch
   hook, the persistent-galaxy-state/descendant bookkeeping, the new memory bound,
   and the MPI implications. Decide opt-in vs. external. Produce a `plan` report.
   This is the highest-leverage item: it promotes ranks 7/9/11/13/14 at once.
2. **Pick one rank-3/4/5 pilot** (recommended: an analytic equilibrium module as a
   fast null-model + a parameter-sweep harness) to validate the
   simulator/data-factory loop end-to-end and benchmark throughput — independent of
   (1) and valuable either way.
3. **Prototype a grey-box module** (rank 4) trained on existing SAM output, and
   ablate it against its analytic sibling (rank 2) to demonstrate the hybrid +
   interpretability capability in one experiment.
4. **Define a provenance-rich mock-output contract** (rank 7) so any future survey
   mock is auditable from the HDF5 alone, before the lightcone layer is built.
5. **Optionally** prototype the Mimic→Mangrove→Mimic loop (ranks 3↔8) as a
   throughput multiplier once (1) is in place.

## Completion Status

Complete for the stated scope: a Mimic-grounded rubric is defined *before*
ranking; 15 methods/applications plus four hybrid opportunities are classified
with summary, gaps, opportunity, Mimic-usage sketch, and references; a ranked
decision table with explicit judgment overrides is provided; a dedicated analysis
of the snapshot-synchronized core pivot (per the user's follow-up) is included; and
the ranking is cross-checked against all seven VISION principles. Not included (and
out of
scope): per-opportunity implementation designs, measured benchmarks, and a
systematic (vs. targeted) literature review — each flagged as a follow-on.
