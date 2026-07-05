---
name: mimic-scientific-method
description: The discipline that turns a hunch into an accepted scientific result in Mimic - evidence design, not test mechanics. Load when a task involves claiming parity, correctness, or improvement of any physics output; comparing Mimic runs against each other, against original SAGE, or across formats/simulations; deciding whether an output difference is a bug, chaos, or acceptable noise; designing a tolerance; regenerating a baseline; float vs double / ULP-level reasoning about numerical differences; conservation checks; investigating "the numbers changed"; or proposing/retiring a physics experiment. Also load before writing any commit message or report that makes a scientific claim.
---

# Mimic Scientific Method

The project's one confirmed non-negotiable evidence bar (project owner, 2026-07-03): **numbers before claims**. No parity, correctness, or improvement claim rests on eyeballed plots — every claim is backed by a measured comparison with stated tolerances. This skill is how to produce those numbers: comparison recipes, tolerance design, precision reasoning, and the discipline for accepting or retiring an idea. The methods here are distilled from Mimic's own completed investigations (full stories: `mimic-failure-archaeology`).

## When to use / when NOT to use

Use for: designing the evidence for any scientific claim, interpreting numerical differences, tolerance and baseline decisions, planning discriminating experiments, adversarial review of your own conclusion.

Do NOT use for:
- Running the test suites or writing tests mechanically — see the `mimic-validation-and-qa` skill.
- The measurement tools themselves (comparators, inspectors, fuzzer) — see the `mimic-diagnostics-and-tooling` skill.
- What the physics means — see the `mimic-sam-reference` skill.
- Whether/how a change is gated — see the `mimic-change-control` skill.

## First actions

1. Write down the claim you intend to make, as a sentence with a number in it, BEFORE running anything ("X is bit-identical for ≥98% of galaxies per property", "conservation holds to 0.1%", "runtime improves ≥20% on the default run").
2. Predict the numbers. If you cannot state what you expect to measure and why, you are not ready to run.
3. Identify the comparison population and the matching key (section 1) — a comparison without stable identity is a plot, not evidence.
4. Decide the tolerance and its justification (section 3) before seeing the result, not after.

## 1. The comparison recipe (Mimic's proven pattern)

The sage16-vs-SAGE parity campaign is the house method; reuse its structure for any two-run comparison:

1. **Fix the configuration.** Audit that both runs use identical parameters, cosmology, snapshot lists, and input trees before comparing outputs. Configuration diffs masquerade as physics diffs.
2. **Match objects by stable identity, never by array order.** Within one Mimic lineage, use `UniqueGalaxyID` per snapshot (the cross-run identity; independent of MPI layout). Across different codes without shared IDs, match by invariant physical tuples — the parity campaign matched on (Pos rounded to 4 dp, Len, Type), which was unique in both outputs with a 100% match rate. Report the match rate; unmatched objects are findings, not discards.
3. **Compare per-property, per-object; aggregate second.** The strongest statistic is the fraction of matched objects bit-identical per property, then percentile relative differences (median, p99). Distribution-level agreement (mass functions) is the WEAKEST evidence — populations can agree while every galaxy is wrong.
4. **Iterate fix-and-measure.** One change, full rerun, re-compare against the unchanged reference. Never batch fixes into one measurement — you lose attribution.
5. **Explain the residuals or you are not done.** The parity endpoint was not "100%": it was ≥98% bit-identical per property, with residuals *identified* as (a) float-ULP noise (~1e-7 relative, p99 ~1e-6) at known float storage boundaries and (b) ~0.1% of galaxies flipping discrete threshold decisions (merge-vs-disrupt) under 1-ULP perturbation — chaotic amplification, equally "correct" in both codes, including an expected one-galaxy count difference at z=0. A residual you can name is a result; a residual you can't is an open bug.

## 2. Conservation, consistency, and sanity checks

Cheap invariants that catch large classes of error; run them before fine-grained comparison:

- **Mass/metal budget**: for any reservoir change, sum sources and sinks across the affected reservoirs; the pipeline moves mass between ColdGas/HotGas/EjectedGas/Stellar/BH/ICS and creates it only via infall. The reincorporation-precision investigation accepted per-halo diffs of 2–12% only after verifying aggregate mass and energy conservation agreed to 0.01–0.22% — redistribution/timing, not creation or loss.
- **Unit consistency**: the core scientific test verifies `Vvir² ≈ G·Mvir/Rvir` to 1% with G taken from the run's own schema — reuse that pattern for any new derived quantity.
- **Range and finiteness**: NaN/Inf anywhere is a hard fail; declared `range:` bounds with `sentinels` exemptions catch sign errors and unit slips (`tests/generated/property_ranges.json` drives this automatically for output properties).
- **Monotonicity/causality spot checks**: cumulative quantities (stellar mass along a main branch) should not decrease except via documented channels (disruption, stripping).

## 3. Tolerance design

A tolerance is a claim about the noise floor; derive it, don't pick it to pass.

| Comparison class | Justified tolerance | Basis |
|---|---|---|
| Same code, same machine, deterministic path | bit-identical (rtol 0) | Nothing legitimate varies |
| Committed baseline, same platform | rtol 1e-6, atol 1e-10 | Float storage ULP accumulation (framework defaults) |
| Same baseline across platforms (macOS↔Linux) | rtol 1e-3 | Measured libm divergence ~7e-4 on the sage16 baseline — this is why CI sets `MIMIC_BASELINE_RTOL=1e-3`; it was measured, not chosen |
| Exact conservation laws | ~1e-10 | Arithmetic roundoff only |
| Approximate physical relations | ~1e-2 | Model discreteness (substeps, thresholds) |
| Literature/observation comparison | ~0.5 (dex-scale) | Systematic uncertainty dominates |

Rules: state the tolerance's basis in the test/report; use a warn band (`warn_rtol`) rather than silently widening a gate; a measurement that fails a strict local gate but passes a relaxed CI gate is a finding to investigate, never a reason to relax locally.

## 4. Precision reasoning (scientific C for this codebase)

The float/double discipline that Mimic's history proves matters:

- **A float stores ~7 significant figures.** Comparisons of nearly-equal physical quantities (`a > b` branch decisions) at float precision can flip on rounding — the core inheritance bug (orphan Rvir/Vvir freezing) was exactly this, invisible until the fields were widened to double.
- **Storage precision ≠ arithmetic precision.** SAGE computes in double locals and stores float; matching it bit-for-bit required Mimic's transport properties to be double so `float += double` arithmetic matched. When mirroring another code, audit the precision of every *intermediate*, not just stored fields.
- **Widening a field is a campaign, not an edit.** Every local copy in every module re-narrows silently (`float vvir = halo->Vvir;`). After widening: grep modules for local copies, then run the FULL suite — the reincorporation re-narrowing was caught only there.
- **Never widen beyond the source data.** Catalog fields carry the precision the catalog wrote (~7 sig figs for ctrees ASCII); a double container adds zero information. Verified once, settled — cite the precedent (`mimic-failure-archaeology`, don't-widen decision) instead of re-running it.
- **ULP-level residuals are a signature, not noise to wave at.** ~1e-7 relative differences localized at known float boundaries = expected; the same magnitude appearing where everything should be double = a re-narrowing bug. Location tells you which.
- **Discrete thresholds amplify chaos.** Any `x > threshold` branch converts 1-ULP input differences into O(1) output differences for individual objects. Quantify how many objects sit on flipped branches; a handful in tens of thousands is expected behavior near thresholds, a systematic drift is not.

## 5. Adversarial refutation and negative results

Before accepting your own conclusion:

1. **One mechanism must explain ALL observations — including the negatives.** The parity campaign's dominant cause (the truncated `1.414`) was accepted because it predicted the observed 3e-4 systematic offset in *every* disk radius AND its downstream fingerprint in the SF threshold; causes that explained only part of the pattern kept hunting.
2. **Ask what your hypothesis forbids.** If your fix is right, which properties must be UNCHANGED? Verify those too. The reincorporation fix was validated both by the ~90 halos that changed *and* by conservation holding on the aggregate.
3. **Write up negative results as decisions.** The ctrees-precision investigation produced no code change — and that "no real gain, don't widen" record is now the citation that stops anyone re-spending that effort. A retired idea gets: what was tried, what was measured, why it lost, and where the evidence lives (commit message, archived report, or `mimic-failure-archaeology`).
4. **Stress the machinery separately from the science.** `scripts/fuzz_pipeline.py` exists to refute "the framework handles any pipeline" claims mechanically (random and dependency-closed module subsets, seeded replay). Use it after core dispatch/validation changes so scientific comparisons aren't polluted by framework bugs.

## 6. The idea lifecycle (as this project actually runs it)

1. **Hypothesis with predicted numbers** (section: First actions).
2. **Cheapest discriminating experiment first** — mini-Millennium or a micro-Uchuu package, one variable at a time; the micro-Uchuu format triplet discriminates reader effects from physics effects.
3. **Evidence at the bar** — sections 1–5; measured, residuals explained, negatives checked.
4. **Adoption through change control** — gates, tests, and (if outputs changed) justified baseline regeneration in the same commit (`mimic-change-control`, `mimic-validation-and-qa`). Structural ideas get a plan in `docs/dev/` first (index: `MIMIC-DEVELOPMENT-PATHWAY.md`).
5. **Or documented retirement** — a written negative result (point 3 above). Silence is the only unacceptable outcome: it guarantees the idea gets re-fought.

Claims about Mimic made externally (papers, READMEs) follow the same bar: state what was measured (e.g. "≥98% of galaxies bit-identical per property against Croton et al. 2016 SAGE on mini-Millennium"), and label anything unmeasured as open/candidate — see `mimic-docs-and-writing`.

## Provenance and maintenance

Method distilled 2026-07-04 from the project's completed investigations: the sage16 parity campaign and report (archived; chronicle in `mimic-failure-archaeology`), the precision-policy commits (`bf0993fa`, `6cbeafe4`, `4a97d3d0`), and the shipped test-framework tolerances. Re-verify the volatile anchors:

```bash
grep -n "BASELINE_RTOL_DEFAULT\|BASELINE_ATOL_DEFAULT" tests/framework/harness.py
grep -n "MIMIC_BASELINE_RTOL" .github/workflows/ci.yml          # measured cross-platform floor
grep -n "warn_rtol" tests/framework/comparison.py | head -3
grep -n "1%" tests/scientific/test_scientific.py | head -3       # virial-relation check tolerance
git log --oneline -n1 bf0993fa 6cbeafe4 4a97d3d0                 # precision-policy anchors resolvable
python3 scripts/fuzz_pipeline.py --help | head -15
```

The doctrine itself (numbers before claims; residuals explained; negatives written up) is owner-confirmed and durable; only the specific tolerance values and tool flags drift.
