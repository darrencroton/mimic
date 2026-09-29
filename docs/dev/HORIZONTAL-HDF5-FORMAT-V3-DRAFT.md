# Mimic Horizontal-HDF5 Format — Version 3 (Promoted)

**Purpose**: Point readers of the former version 3 draft at the normative version 3 specification, which now lives in [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md#version-3).

> **Status: superseded by promotion on 2026-09-29.** This file held the version 3 draft specification, which was normative for the producer from Gate G1 (2026-09-23; [`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md)). Once Mimic's reader and driver consumed version 3 and the real-data parity gate had passed, its text was moved into [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md) as the normative [Version 3](HORIZONTAL-HDF5-FORMAT.md#version-3) section, per decision R0-11 of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md). The contract did not change in the move. This file specifies nothing; it is kept because code comments and committed documents name it and its sections.

---

## Table of Contents

1. [Section Map](#section-map)
2. [Runtime support status](#runtime-support-status)

---

## Section Map

Code comments and documents cite the draft by section title. Each old title now lives here:

| Draft section | Normative section in `HORIZONTAL-HDF5-FORMAT.md` |
|---|---|
| Role and Scope | [V3 Role and Scope](HORIZONTAL-HDF5-FORMAT.md#v3-role-and-scope) |
| What Version 3 Changes | [What Version 3 Changes](HORIZONTAL-HDF5-FORMAT.md#what-version-3-changes) |
| File Set and Naming | [V3 File Set and Naming](HORIZONTAL-HDF5-FORMAT.md#v3-file-set-and-naming) |
| Header Attributes | [V3 Header Attributes](HORIZONTAL-HDF5-FORMAT.md#v3-header-attributes) |
| Halo Datasets (Topology, Identity, Payload) | [V3 Halo Datasets](HORIZONTAL-HDF5-FORMAT.md#v3-halo-datasets) ([Topology](HORIZONTAL-HDF5-FORMAT.md#v3-topology), [Identity](HORIZONTAL-HDF5-FORMAT.md#v3-identity), [Payload](HORIZONTAL-HDF5-FORMAT.md#v3-payload)) |
| The Schema Group | [V3 Schema Group](HORIZONTAL-HDF5-FORMAT.md#v3-schema-group) |
| Link Scope | [V3 Link Scope](HORIZONTAL-HDF5-FORMAT.md#v3-link-scope) |
| Format Invariants | [V3 Format Invariants](HORIZONTAL-HDF5-FORMAT.md#v3-format-invariants) (numbering unchanged) |
| Ordering Contracts | [V3 Ordering Contracts](HORIZONTAL-HDF5-FORMAT.md#v3-ordering-contracts) |
| Source Identity | [V3 Source Identity](HORIZONTAL-HDF5-FORMAT.md#v3-source-identity) |
| Galaxy Identity Encoding | [V3 Galaxy Identity Encoding](HORIZONTAL-HDF5-FORMAT.md#v3-galaxy-identity-encoding) |
| Forest Sidecar | [V3 Forest Sidecar](HORIZONTAL-HDF5-FORMAT.md#v3-forest-sidecar) |
| Validation Requirements | [V3 Validation Requirements](HORIZONTAL-HDF5-FORMAT.md#v3-validation-requirements) |
| Storage Layout | [V3 Storage Layout](HORIZONTAL-HDF5-FORMAT.md#v3-storage-layout) |
| Runtime support status | [V3 Runtime Support](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support) (see below) |
| Versioning Policy | [V3 Versioning Policy](HORIZONTAL-HDF5-FORMAT.md#v3-versioning-policy) |

## Runtime support status

The draft's runtime-status section, written while Mimic could not read version 3, said runtime support was pending. That is no longer true. Mimic's `horizontal_hdf5` reader and horizontal driver consume version 3, and a route is supported only where a recorded parity gate has passed. The routes, their evidence and their limits (including that full Uchuu is not runnable) are listed in [V3 Runtime Support](HORIZONTAL-HDF5-FORMAT.md#v3-runtime-support), with the evidence in [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md).

---

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [VISION.md](../VISION.md): architectural principles and design boundaries
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the normative specification of versions 2 and 3
- [MIMIC-V3-CONSUMER-DESIGN-REVIEW.md](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md): the Gate G1 consumer-design review of version 3
- [MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md](MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md): the runtime parity evidence
