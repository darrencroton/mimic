# Mimic Distributed Snapshot-Global Operations Plan

**Status:** Requirements brief. Split out of `MIMIC-DUAL-DRIVER-PLAN.md` (its former Phase 7) in the 2026-07-02 joint review. The single-node snapshot driver passed its cross-format identity gate on 2026-08-12, so that precondition is met; now blocked only on at least one snapshot-global module contract existing (`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`) — there is nothing to distribute until then. Not scheduled.
**Date:** 2026-07-02

---

## Goal

Add MPI/domain decomposition and cross-domain communication for snapshot-global operations (global abundance ranking, environment measures, synchronous radiation fields, lightcone assembly) after the single-node snapshot driver is correct and validated.

## Constraints carried from the dual-driver work

- Output identity must remain deterministic across task counts (standing constraint in `MIMIC-DEVELOPMENT-PATHWAY.md`).
- Stochastic modules seed from stable per-halo/per-FoF keys, never traversal-order RNG streams.
- The snapshot-HDF5 format is the input contract; any domain decomposition (spatial or forest-sharded) is a driver concern layered over the same files, not a new format.
- Shin-Uchuu is expected to fit single-node on the current hardware (the former "~300–450 GB peak estimated" figure is **superseded** — it predates the decision to retain a second complete raw slab, and the peak has not yet been recomputed; see `POST-PHASE-5-WORK.md` §2.2); the first concrete driver for this plan is likely a larger simulation (e.g. full Uchuu snapshot slabs approach 10⁸–10⁹ halos) or wall-clock pressure, not Shin-Uchuu itself.

## Gate (when activated)

Distributed results match the single-node reference within a documented tolerance on a reference box.
