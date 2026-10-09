"""The horizontal-dataset rewriter: ``rewrite_trees.py``'s passes.

One tool reads a horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue
slab by slab and writes a version 3 dataset of the same halos under a forest
table (``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, Version 3). Two
operations are implemented here:

- **migrate**: a version 2 input becomes a version 3 dataset. Every identity
  (``ForestIndex``, ``HaloRankInForest``, ``SourceHaloID``) is computed from
  the version 2 columns, every row is placed at its (``ForestIndex``,
  ``HaloRankInForest``) position, the int32 links become int64 links with
  their target-snapshot companions, ``/schema`` is built from the package's
  converter profile, and the sidecar ordinals come from the index files.
- **noop**: a version 3 input is rewritten under the identity table and must
  reproduce its input in every dataset value, dtype, header attribute,
  ``/schema`` attribute and sidecar value. It is the permanent self-test of the
  passes below.

Only datasets whose every ``Descendant`` link is adjacent (``links_adjacent``
1) are accepted; the gapped case is out of scope and refused.

**Passes.** Snapshots are processed in descending order, because the
reference rank orders a forest's halos by descending snapshot first.

0. *Pass 0* (``rewriter.pass0``): the headers, the sidecar, the index files
   (``forests.list``, ``locations.dat``) and their per-forest table, the
   per-forest halo totals from a scan of every slab's ``ForestIndex``, the
   terminal halos (every ``Descendant == -1``) as the trees' roots, the
   correspondence of those roots with the index files' tree roots (a
   mismatch is refused, never repaired), and the per-tree inventory. The
   totals give each forest's ``SourceHaloID`` base (a prefix sum from 1,
   refused on int64 overflow) and ``max_halo_rank_in_forest``.
1. *Pass 1* (``rewriter.passes.rank_slab``), per slab with a per-forest
   running count: the reference rank key (``ForestIndex`` ascending, FoF
   central's ``MostBoundID`` ascending, -1 for a central else that id
   ascending, own ``MostBoundID`` ascending) gives the slab's new row order;
   ranks continue each forest's running count; the three identity columns
   follow; every halo is labelled with its tree (its descendant's label in
   the next slab, else its own ``MostBoundID``'s root ordinal). The
   permutation and identities go to the remap store, of which only slabs
   N-1, N and N+1 are resident.
2. *Pass 2* (``rewriter.passes.write_slab`` and ``verify_slab``), trailing
   pass 1 by one slab: the payload is gathered into the new order, every link
   is remapped through its target slab's permutation (``Descendant`` at N+1,
   ``FirstProgenitor`` at N-1, the rest at N) with its companion snapshot
   column -1 iff the link is -1, the file is written to a temporary name,
   renamed on success, re-read and verified against the input.

**Output.** A converter-shaped workdir: ``manifest.json`` of the shape the
producer battery binds to, ``dataset/`` holding exactly the snapshot files
and ``forests.h5``, and beside the manifest ``tree_inventory.npy``,
``rewrite_state.json`` (the restart state) and ``rewrite_summary.json``.
``convert_trees.py validate --workdir <output>`` and ``report`` then run with
their binding and conservation checks applied.

**Memory**, with ``n`` the widest slab's rows, ``n_next`` the next slab's,
``F`` ``n_forests_total``, ``T`` the number of trees and ``P`` the index's
(file, forest) pairs (Shin-Uchuu: n = 519,342,987, F = 166,547,771,
T = 315,004,242, P about 320 million); block reads add ``8 B x block_rows``
per column read:

- pass 0: the index, 32 B x T, after a load that peaks at 79 B x T; its
  forest table, 40 B x F + 8 B x P, built with a transient of up to 81 B x T;
  the terminal roots, 20 B x T at rest after a 42 B x T peak; the per-forest
  totals and bases, 16 B x F; the per-tree inventory, 56 B x T. About 44 GB
  at Shin-Uchuu, dominated by the index load.
- pass 1, per slab: at the sort, the four key columns (``ForestIndex``,
  upid, pid, ``MostBoundID``) and the sort's index and workspace, about
  48 B x n; after it, ``MostBoundID``, the permutation, the sorted forest,
  the rank, the id and one further input column at a time (the held rank,
  then ``Descendant``), 48 B x n, plus the two label arrays, 4 B x (n +
  n_next) and one-byte masks: at most about 56 B x n + 4 B x (n + n_next),
  plus the running count, 8 B x F, the roots, 8 B x T, and the tree counts,
  8 B x T; about 36 GB at Shin-Uchuu.
- the remap store: 32 B per halo of each resident slab (the permutation and
  three identities), at most three slabs: 96 B x n, about 50 GB; its peak is
  recorded in the summary.
- pass 2, per slab: while writing, one input column and its gathered output
  at a time (at most 12 B x n each) beside the three resident permutations
  and their inverses, 48 B x n; while verifying, the three slabs' input
  ``MostBoundID``, 24 B x n, the written and the input link column, the
  compared target ids (32 B x n) and the three identity columns (24 B x n)
  beside the permutations and inverses: at most about 104 B x n, about
  54 GB.

Every whole-slab array is int64-indexed; nothing is narrowed to int32 but the
tree labels, whose ordinals the root enumeration bounds.
"""

#: The rewriter's own version, recorded in every manifest it writes.
REWRITER_VERSION = 1

#: The one source format the rewriter accepts and stamps.
SOURCE_FORMAT = "consistent_trees_ascii"

OPERATION_MIGRATE = "migrate"
OPERATION_NOOP = "noop"
OPERATIONS = (OPERATION_MIGRATE, OPERATION_NOOP)

#: Files the rewriter writes beside the manifest, never inside ``dataset/``.
STATE_NAME = "rewrite_state.json"
SUMMARY_NAME = "rewrite_summary.json"
TREE_INVENTORY_NAME = "tree_inventory.npy"
DATASET_DIR = "dataset"

__all__ = [
    "DATASET_DIR",
    "OPERATIONS",
    "OPERATION_MIGRATE",
    "OPERATION_NOOP",
    "REWRITER_VERSION",
    "SOURCE_FORMAT",
    "STATE_NAME",
    "SUMMARY_NAME",
    "TREE_INVENTORY_NAME",
]
