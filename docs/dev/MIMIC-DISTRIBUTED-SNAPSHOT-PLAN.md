# Mimic Distributed Snapshot-Global Operations Plan

**Status:** Requirements brief. Split out of `MIMIC-DUAL-DRIVER-PLAN.md` (its former Phase 7) in the 2026-07-02 joint review. The single-node snapshot driver passed its cross-format identity gate on 2026-08-12, so that precondition is met; now blocked only on at least one snapshot-global module contract existing (`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`) — there is nothing to distribute until then. Not scheduled.
**Date:** 2026-07-02

---

## Goal

Add MPI/domain decomposition and cross-domain communication for snapshot-global operations (global abundance ranking, environment measures, synchronous radiation fields, lightcone assembly) after the single-node snapshot driver is correct and validated.

## Constraints carried from the dual-driver work

- **Serial partitioned snapshot output — delivered, not by this plan (recorded 2026-08-13).** This plan was nominally assigned "partitioned snapshot output" when it split out of `MIMIC-DUAL-DRIVER-PLAN.md`, but its text never recorded that obligation. D5(a) delivers the serial case: a serial (`NTask == 1`) snapshot-ordered run already writes one HDF5 partition file per requested output snapshot, named by that snapshot's number, plus a master — see `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` and `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver". This supersedes this plan's nominal assignment for the serial case. What remains here is genuinely distributed: spreading snapshot-global work (and, if needed, further partitioning) across MPI tasks once at least one snapshot-global module contract exists — not introducing per-snapshot output partitioning, which is done.
- Output identity must remain deterministic across task counts (standing constraint in `MIMIC-DEVELOPMENT-PATHWAY.md`).
- Stochastic modules seed from stable per-halo/per-FoF keys, never traversal-order RNG streams.
- The snapshot-HDF5 format is the input contract; any domain decomposition (spatial or forest-sharded) is a driver concern layered over the same files, not a new format.
- Shin-Uchuu is **expected** to fit single-node on the current hardware, but this is not yet settled. **Recomputed 2026-08-13 from measured struct sizes** (`POST-PHASE-5-WORK.md` §2.2): ≈**317 GB** against 512 GB installed for the `sage16` production configuration under its measured output ratio, clear of the 85% fallback trigger (≈435 GB) — but the galaxy pool's allocation high-water is unmeasured, and the peak reaches ≈428 GB if the processed buffer and pool both grow to 1.5× the slab. Treat single-node feasibility as **pending the rehearsal measurement**, not as established. This still supersedes the old "~300–450 GB estimated" figure, which predated the decision to retain a second complete raw slab. The first concrete driver for this plan is likely a larger simulation (e.g. full Uchuu snapshot slabs approach 10⁸–10⁹ halos) or wall-clock pressure rather than Shin-Uchuu itself — unless that measurement says otherwise.

## Why this arrives sooner than the scale trigger suggests

**Recorded 2026-08-20: running Shin-Uchuu somewhere other than the conversion machine is a third driver, and the nearest one.** The single-node projection above is against 512 GB installed on one specific host. Shin-Uchuu is intended to be run by students on OzSTAR, where the per-node memory is smaller and work is allocated across nodes — so distribution is a *deployment* requirement here, not only a scale one, and it does not wait for a bigger simulation.

Note the boundary precisely, because it decides whether this plan is the right lever at all. MPI decomposition serves the **cluster** case: more nodes, each holding a subdomain. It does **not** reduce what a single low-memory workstation needs, since every rank still lives in that machine's RAM and ghost regions add duplication. For a student on one workstation the relevant levers are the subset dataset the Shin-Uchuu rehearsal already produces, and the compact previous-slab projection recorded as the memory fallback in `POST-PHASE-5-WORK.md` §2.2 — not this plan.

**Checked against exactly this boundary, 2026-09-04 (`SHIN-UCHUU-CONVERSION-PLAN.md` → "P3b, P4 done and measured").** Shin-Uchuu's production `sage16` run hit a real single-node memory wall (≈639–697 GB projected against ≈540 GB available), and this plan was evaluated as a candidate fix and ruled out for exactly the reason above: it targets cluster deployment, not a single low-memory workstation, and is unbuilt regardless (gated behind `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`, itself unbuilt). The run relocated to an NT large-memory node instead. A **separate, non-overlapping approach** — chunked/streaming snapshot-slab processing to cap per-process peak memory regardless of host count — is recorded as a concept note in [`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md). It complements rather than substitutes for this plan: a decomposed rank could itself use chunked/streamed slabs internally, but the reverse motivation — a single-node memory bound, no cluster required — is what that note exists for, and is the more urgent of the two given repeated Shin-Uchuu use and larger future simulations are both expected.

## Constraint carried backwards from the performance work

**Do not assume single-threaded ranks. Recorded 2026-08-20.** [`OPTIMISATION-SPECTRUM.md`](OPTIMISATION-SPECTRUM.md) item 16 (thread-per-forest) and item 18 (snapshot-slab parallelism) are live candidates scheduled as pathway step 4, after this work. Whether a rank is threaded changes the per-rank memory budget, ghost-region duplication, the load-balance unit, and the required `MPI_THREAD_*` support level — implementation commitments, not knowledge that can be deferred. **State the assumed thread model explicitly in the first design sketch.** Note also that the enabling refactor for item 16 — instancing the tree-driver globals listed in that item — is substantially the same refactor a rank-parallel design wants, and it is already half done (the galaxy pool is an instanced handle API; the snapshot driver holds its state in a struct). If that sketch finds the thread model genuinely blocking, say so and revisit the pathway ordering rather than working around it.

## Gate (when activated)

Distributed results match the single-node reference within a documented tolerance on a reference box.
