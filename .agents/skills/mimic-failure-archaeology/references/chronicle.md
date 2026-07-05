# Mimic Chronicle — Eras and Settled Incidents

Full record backing `SKILL.md`. Each incident is recorded as symptom → root cause → evidence → status. Verify any hash with `git show -s --format='%h %ad %s' --date=short <hash>` before citing it in new work. Status snapshot: 2026-07-03.

## Era table

| Era | Period | What happened |
|---|---|---|
| Upstream SAGE | 2012–2025 | Original SAGE semi-analytic model development; the legacy history Mimic's repo inherits (dead ends from this era are recorded below) |
| DM-only pivot + rebrand | 2025-10/11 | Codebase pivoted to a dark-matter-halo framework and rebranded to Mimic |
| Module/metadata system | 2025-11/12 | Runtime module system, YAML metadata, and code generation built |
| SAGE re-import | 2025-12 | SAGE physics re-imported as modules on the new framework |
| v0.1-beta | 2025-12-23 | First tagged beta (`git tag`: `v0.1-beta`) |
| Package split | 2026-05 | Model packages (`models/<model>/`) and simulation packages (`simulations/<sim>/`) separated |
| sage16 rename + parity | 2026-06 | Model renamed sage→sage16; parity campaign vs original SAGE (incident 1) |
| Unit contract | 2026-06 | Fixed internal reference units + generated unit registry at the reader boundary |
| Readers: ctrees + micro-uchuu | 2026-06 | Consistent-Trees ASCII/HDF5 readers and micro-uchuu packages landed |
| Galaxy-ID + chunked output | 2026-06 | UniqueGalaxyID scheme and chunked output files (`pre/post-chunk-baseline` marker branches) |
| Style sweep 1–18 | 2026-06 | Eighteen numbered style-sweep batches with debt checkpoints, leading into v1.0 |
| v1.0 | 2026-06-29 | Release tag (`git tag`: `v1.0`) |
| Dynamic timestep + precision | 2026-06/07 | Dynamic substeps shipped (incident 7); precision policy consolidated (incidents 2–4) |
| Snapshot planning | 2026-07 | Snapshot-ordered driver and related plans in progress — see `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` |

Tags marking era boundaries: `v0.1-beta`, `v0.5`, `v0.9-pre-release`, `v1.0` (dates via `git show -s --format='%h %ad %s' --date=short <tag>`).

## Incident 1 — sage16 parity campaign (closed 2026-06-11)

**Symptom.** Mimic's sage16 output disagreed with original SAGE per-galaxy, per-property, beyond noise.

**Root causes (six, in rough order of impact).**
1. **Literal `1.414`** — SAGE uses the literal `1.414` where √2 belongs; Mimic used the accurate constant. Dominant divergence source. sage16 now mirrors the literal, marked `// SAGE parity:`.
2. **Reionization H(z) 1/h bug** — a genuine physics bug in the Hubble-rate expression used by the reionization recipe (a stray 1/h). Mirrored for parity in sage16; a corrected model package should fix it.
3. **Metallicity helper mismatches** — float vs double arithmetic and guard-condition differences in the metallicity helper functions.
4. **Epsilon/threshold mismatches** — comparison-epsilon and threshold differences, including the Krumholz–Dekel recipe using the central's Mvir where Mimic used a different mass.
5. **Metal-yield ordering** — SAGE applies metal enrichment AFTER disk instability; Mimic applied it earlier. Fixed by introducing a new module, `sage_apply_metal_enrichment`, ordered after disk instability in the pipeline.
6. **float32 transport** — transport properties carried as float32 (including core dT) truncated intermediate state; widened to double.

**Evidence.** Per-galaxy, per-property bitwise comparison against original SAGE output; galaxies matched by the tuple (rounded Pos, Len, Type). Result: ≥98% bit-identical per property. Residuals are ULP-level differences plus ~0.1% of galaxies flipping across chaotic thresholds. A one-galaxy difference in the z=0 galaxy count is EXPECTED and documented — do not file it as a regression. Full report was archived to `archive/SAGE16-PARITY-REPORT.md` (gitignored, machine-local; may not exist on your machine).

**Status.** Closed. The comparison method (match key, tolerances, residual taxonomy) is the template for any future parity claim — see the `mimic-scientific-method` skill.

## Incident 2 — Precision policy (`bf0993fa`, closed)

**Symptom.** While widening core properties to double for parity work, an inheritance comparison bug surfaced: with float precision, rounding could make a comparison freeze orphan Rvir/Vvir updates — a real correctness bug that float arithmetic had been masking.

**Root cause.** Float rounding in an inheritance-time comparison; precision policy previously ad hoc.

**Evidence.** Commit `bf0993fa` ("Widen core/simulation precision to double; make dynamic-substep cap configurable", 2026-07-01) — the widening exposed and fixed the bug together.

**Status.** Closed; policy is doctrine: core/simulation shared properties default to double for all models; sage16 reservoirs stay float ONLY for SAGE parity; catalog fields match the SOURCE data's precision. Never inherit one model's parity float choices into core. Forward-looking rules live in the `mimic-properties` skill.

## Incident 3 — Stale float locals after widening (`6cbeafe4`, closed)

**Symptom.** After the double-precision widening, the full test suite flagged per-halo diffs traced to `models/sage16/modules/sage_reincorporation/sage_reincorporation.c`.

**Root cause.** Local `float` variables copied from the newly widened struct fields silently re-narrowed the values. Targeted tests missed it; only the FULL suite caught it.

**Evidence.** Commit `6cbeafe4` ("Fix stale float precision in reincorporation; regenerate physics baseline", 2026-07-01). The resulting per-halo diffs were verified as a legitimate precision cascade: ~90 corrected halos, mass-conservation deltas 0.01–0.22%, then the physics baseline was regenerated.

**Status.** Closed. Lesson: a struct-field type change is incomplete until every local copy, temporary, and helper signature is chased; run the full suite, not just the touched module's tests.

## Incident 4 — ctrees catalog precision: DON'T widen (`4a97d3d0`, closed)

**Symptom.** Question raised: should Consistent-Trees catalog fields also be widened to double under the new policy?

**Root cause of the "no".** The ASCII source data carries only ~7 significant figures — doubling the storage adds no information — and `ctrees_compat.h` is deliberately float to stay byte-compatible with upstream tooling.

**Evidence.** Commit `4a97d3d0` ("Investigate Consistent-Trees catalog precision: no real gain, don't widen", 2026-07-01) records the investigation and decision.

**Status.** Closed. This is the precedent to cite before reopening any "widen the catalog fields" proposal: catalog fields match source precision, full stop.

## Incident 5 — Docs overhaul revert and redo (`8d0f39c6` → `432e4ca7`, closed)

**Symptom.** A large documentation overhaul (`332153c7`, "Overhaul documentation: narrative restructure and sage16 path fixes throughout") was reverted wholesale (`8d0f39c6`), then redone the same day as `432e4ca7` ("Overhaul documentation: framework-first narrative and sage16 path fixes").

**Root cause.** The first overhaul's framing did not put the framework first. The doctrine: Mimic is a physics-agnostic framework; models (sage16, sham, halos-only) are interchangeable packages, not the product.

**Evidence.** Commits `332153c7` → revert `8d0f39c6` → redo `432e4ca7` (all 2026-06-11); the redo also fixed real rename staleness found during review (sage16 path fixes throughout the docs).

**Status.** Closed. Framing rules live in the `mimic-docs-and-writing` skill; this incident is why they exist.

## Incident 6 — Rename staleness (sage→sage16, closed)

**Symptom.** After the sage→sage16 model rename (`3c40e2b5`, 2026-06-09), scattered breakage kept surfacing: `first_run.sh` had a broken `RUN_FILE` default, ~70 stale references remained across docs and scripts, and the full-physics baseline test silently skipped for a while because its model-name guard still checked for "sage".

**Root cause.** The rename swept code but not docs, scripts, test-harness defaults, or model-name guard strings inside tests. A guard mismatch does not fail — it SKIPS, which reads as green.

**Evidence.** Rename commit `3c40e2b5`; stale-reference sweeps in the doc overhauls `332153c7`/`432e4ca7`; `a646e7b4` ("Refresh test baselines after sage16 parity changes; re-enable physics baseline test") re-enabled the silently-skipping baseline test.

**Status.** Closed. Rename checklist (grep docs + scripts + harness defaults + test guards; audit SKIP counts before/after) is encoded in the `mimic-change-control` skill.

## Incident 7 — Dynamic timestep campaign (shipped 2026-07-01, closed)

**Symptom/goal.** Fixed `SubSteps` under- or over-resolves halos with very different dynamical times.

**Outcome.** Dynamic per-FoF substeps: `TimestepScheme: dynamic`, substeps = ceil(dt·SubSteps/t_dyn) with t_dyn = Rvir/Vvir, clamped to `MaxDynamicSubsteps`. The clamp default moved 50→200 because convergence MEASUREMENT showed 50 truncated early high-z halos.

**Evidence.** Commits `469b7adc` (config plumbing), `b942bf3c` (computation), `58f1d3c2` (provenance/metadata). Planning and convergence notes were archived: recover via `git show 9a6f4322^:docs/dev/DYNAMIC-TIMESTEP-CONVERGENCE-NOTES.md` (deleting commit `9a6f4322`; sibling plan docs deleted in `c568e8bb` — list with `git show --stat 9a6f4322` / `git show --stat c568e8bb`).

**Status.** Shipped and closed. Runtime usage is in the `mimic-config-and-flags` skill.

## Incident 8 — Stripping metal conservation (documented `25f54878`, fixed `3f1e124b`, closed)

**Symptom.** Metals were not conserved through the satellite gas-stripping path.

**History.** First documented as a known issue (`25f54878`), later fixed (`3f1e124b`). If you find old notes describing the issue as open, check these hashes before "rediscovering" it — the fix postdates the documentation.

**Status.** Closed (fixed). Conservation-check methodology: see the `mimic-scientific-method` skill.

## Incident 9 — fix_flybys z=0 topology divergence (`b727fd36`, accepted)

**Symptom.** The three micro-uchuu packages (binary, HDF5, ASCII) produce byte-identical output at every snapshot EXCEPT the final one.

**Root cause.** The Consistent-Trees ASCII reader's `fix_flybys` step collapses multiple z=0 FoF groups (flyby halos, negated MostBoundID) into one; the L-Halo binary and HDF5 readers do not.

**Evidence.** Commit `b727fd36` documents the divergence and the acceptance rationale.

**Status.** Accepted divergence, NOT a bug. Do not "fix" one reader to match another without a plan-level decision; see the `mimic-simulations-and-readers` skill.

## Incident 10 — Deliberate retirements (not failures)

These were removed on purpose after the design moved on. Restoring them is regression, not recovery.

| Hash | What was retired | Why |
|---|---|---|
| `53fb1904` | Split-pass merger path | Superseded by the single-pass pipeline design |
| `8be9309f` | Per-task partition model | Superseded by per-file/enumerated partition schemes |
| `74c56f4a` | Central parameter registry | Superseded by module-local parameter validation in each `init()` |

## Upstream SAGE-era dead ends (prior art)

From the pre-Mimic history; check before proposing similar physics.

| Hash | Dead end |
|---|---|
| `3a2d1f30` | Abandoned Len-based infall recipe |
| `b0088922` | Reverted infall recipe change |
| `10ac96eb` | Ejected-gas wind-back experiment |

## Standing warnings

- **`feature/positive-agn-feedback`** — stalled branch: 1 commit ahead, ~246 behind, predates the package split. Re-port, never merge.
- **`pre-chunk-baseline` / `post-chunk-baseline`** — ancestor marker branches around the chunked-output change; not for development.
- **`archive/`** — gitignored machine-local symlink; full reports there (parity report, fuzz logs) are corroboration only. The durable record is this chronicle plus git history (`git show <hash>^:<path>` for deleted docs).
- **`// SAGE parity:` comments** — deliberate-quirk markers in sage16, concentrated in `models/sage16/` with a few core-side parity notes; recount with `rg -n 'SAGE parity' models/sage16/ src/core/ | wc -l`. Never fix in place; fork a new model package.
