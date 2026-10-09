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
   re-read and verified against the input, and renamed on success.

**Output.** A converter-shaped workdir: ``manifest.json`` of the shape the
producer battery binds to, ``dataset/`` holding exactly the snapshot files
and ``forests.h5``, and beside the manifest ``tree_inventory.npy``,
``rewrite_state.json`` (the restart state) and ``rewrite_summary.json``.
``convert_trees.py validate --workdir <output>`` and ``report`` then run with
their binding and conservation checks applied.

**Memory.** The concurrent peak per phase, with ``n`` the widest slab's
rows, ``F`` ``n_forests_total``, ``T`` the index's trees, ``P`` its (file,
forest) pairs and ``b`` the rows read per block (Shin-Uchuu: n = 519,342,987,
F = 166,547,771, T = 315,004,242, P about 320 million). The per-row and
per-tree coefficients marked measured were measured with ``tracemalloc`` on a
synthetic 400,000-row slab (``rewriter.inputs.MEMORY_FORMULAS`` repeats them
for the summary and ``planned_peak_bytes`` evaluates them for an input):

- pass 0 peaks at its inventory step, **133 B x T + 80 B x F + 8 B x P +
  24 B x b**: the index (32 B x T, after a load that peaks at 79 B x T), its
  forest table (40 B x F + 8 B x P, built with a transient of 57 B x T at
  Shin-Uchuu's ratio and 81 B x T when forests equal trees), the sidecar
  columns (24 B x F), the roots and their slabs (12 B x T, after a gathering
  peak of 42 B x T), the totals and bases (16 B x F), and the inventory
  (56 B x T) with the marker ordinals' transient (33 B x T, measured); about
  58 GB at Shin-Uchuu. The index, forest table, sidecar columns, totals and
  inventory are then released (the inventory is checkpointed in the scratch
  and reloaded at the end).
- the baseline through passes 1 and 2 is **16 B x F + 16 B x T + 4 B x n**:
  the forest bases and running counts, the roots and tree counts, and the
  previous slab's labels; about 9.8 GB.
- the remap store: 32 B per row of each resident slab (the permutation and
  three identities), at most three slabs, **96 B x n**; about 50 GB. Its peak
  is recorded in the summary.
- pass 1 holds, beside the baseline and the two remap entries already
  resident (64 B x n), a working set of **68 B x n**, and **79 B x n** for a
  last slab whose every halo is terminal (measured: the sort's four key
  columns and workspace, then the identities, the labels and the root
  lookup's transients); about 80 GB concurrent at Shin-Uchuu.
- pass 2 holds, beside the baseline and three remap entries, **45 B x n**
  while writing (one input column and its gathered output, or a link's
  temporaries and the target's inverse permutation) and **79 B x n** while
  verifying (the written and input link, the compared target ids, one target
  slab's ``MostBoundID``, then the identity columns), both measured; about
  101 GB concurrent at Shin-Uchuu, the run's peak.

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
