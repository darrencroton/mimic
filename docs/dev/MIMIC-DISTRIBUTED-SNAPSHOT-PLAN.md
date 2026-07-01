# Mimic Distributed Snapshot-Global Operations Plan

**Status:** Requirements brief. Split out of `MIMIC-DUAL-DRIVER-PLAN.md` (its former Phase 7) in the 2026-07-02 joint review. Blocked on the single-node snapshot driver passing its cross-format identity gate and on at least one snapshot-global physics contract existing. Not scheduled.
**Date:** 2026-07-02

---

## Goal

Add MPI/domain decomposition and cross-domain communication for snapshot-global operations (global abundance ranking, environment measures, synchronous radiation fields, lightcone assembly) after the single-node snapshot driver is correct and validated.

## Constraints carried from the dual-driver work

- Output identity must remain deterministic across task counts (standing constraint in `MIMIC-DEVELOPMENT-PATHWAY.md`).
- Stochastic modules seed from stable per-halo/per-FoF keys, never traversal-order RNG streams.
- The snapshot-HDF5 format is the input contract; any domain decomposition (spatial or forest-sharded) is a driver concern layered over the same files, not a new format.
- Shin-Uchuu fits single-node on the current hardware (~300–450 GB peak estimated); the first concrete driver for this plan is likely a larger simulation (e.g. full Uchuu snapshot slabs approach 10⁸–10⁹ halos) or wall-clock pressure, not Shin-Uchuu itself.

## Gate (when activated)

Distributed results match the single-node reference within a documented tolerance on a reference box.
