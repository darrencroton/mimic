"""The horizontal-dataset rewriter: ``rewrite_trees.py``'s passes.

One tool reads a horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue
slab by slab and writes a version 3 dataset of the same halos under a forest
table (``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, Version 3). Three
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
- **cut** (``rewriter.cut``): a version 3 input is rewritten under a declared
  cut forest table (a ``forests.list``-shaped file, ``census/cut_table.py``,
  whose every invariant is checked on read). Every tree joins the table's
  forest; a forest the table cuts yields its largest piece under the original
  id and fresh pieces under ids above the catalogue's maximum, contiguous from
  that maximum plus one, which take ``ForestIndex`` ``n_forests_total + k``
  after every original forest. Hosts are resolved under the original
  partition (the FoF central the dataset records) and every halo whose
  central's forest differs from its own is promoted to a central of its own
  FoF group; no host relation is invented. For every forest the table
  changes, FoF chains, ``HaloRankInForest`` and progenitor chain order are
  recomputed under the reference rules; every other forest reproduces its
  payload, topology and ``UniqueGalaxyID`` exactly. The logical descendant
  relation never changes. A table that changes nothing produces the no-op's
  output.

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
   refused on int64 overflow) and ``max_halo_rank_in_forest``. For a cut, a
   descending labelling pre-pass then gives each tree's halo total, the
   table is read and checked against the index and those totals, and the
   output's forests, bases, sidecar and bounds are derived from it before
   any write (``rewriter.cut``).
1. *Pass 1* (``rewriter.passes.rank_slab``), per slab with a per-forest
   running count: the reference rank key (``ForestIndex`` ascending, FoF
   central's ``MostBoundID`` ascending, -1 for a central else that id
   ascending, own ``MostBoundID`` ascending) gives the slab's new row order;
   ranks continue each forest's running count; the three identity columns
   follow; every halo is labelled with its tree (its descendant's label in
   the next slab, else its own ``MostBoundID``'s root ordinal). The
   permutation and identities go to the remap store, of which only slabs
   N-1, N and N+1 are resident. For a cut the labels come first, each halo's
   forest is its tree's piece, a promoted halo takes the key of a central,
   and the progenitor chains over the slab (its ``NextProgenitor`` and the
   next slab's ``FirstProgenitor``) are recomputed by the reference
   insertion loop over the new encounter order with ``M_Crit200`` and kept
   in the remap entry.
2. *Pass 2* (``rewriter.passes.write_slab`` and ``verify_slab``), trailing
   pass 1 by one slab: the payload is gathered into the new order, every link
   is remapped through its target slab's permutation (``Descendant`` at N+1,
   ``FirstProgenitor`` at N-1, the rest at N) with its companion snapshot
   column -1 iff the link is -1, the file is written to a temporary name,
   re-read and verified against the input, and renamed on success. For a cut
   the FoF chains are the runs of the new order (each group its central then
   its members, a promoted halo a group of one) and the progenitor links
   come from the remap entries; the verification then proves that every
   descendant carries the ``MostBoundID`` it carried in the input, that every
   untouched forest reproduces its payload, links and identity exactly, that
   every promoted halo is its own group's first halo, and that the
   recomputed chains partition the progenitors and the groups.

**Output.** A converter-shaped workdir: ``manifest.json`` of the shape the
producer battery binds to, ``dataset/`` holding exactly the snapshot files
and ``forests.h5``, and beside the manifest ``tree_inventory.npy``,
``rewrite_state.json`` (the restart state), ``rewrite_summary.json`` and, for
a cut, ``cut_pieces.npy`` (every piece of every split forest).
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

A cut adds (``rewriter.cut``), with ``F'`` the output's forests and ``p`` the
progenitors of the widest slab (Shin-Uchuu under the decided table: F' =
244,953,607, T = 315,004,242; the coefficients are counted from the arrays
held, not measured):

- the labelling pre-pass, **8 B x n + 16 B x b** beside pass 0's resident
  arrays (two slabs' labels and one block of ``Descendant`` and
  ``MostBoundID``), and the per-tree totals, 8 B x T;
- the table's installation, transient: the table read and checked at the
  census reader's **128 B x T** (about 40 GB), then the per-tree new forest
  (8 B x T, resident through the passes, about 2.5 GB), the output's totals
  and bases (16 B x F') and the pieces (56 B per piece of a split forest);
- the baseline grows by 8 B x T + 16 B x (F' - F);
- the remap store holds two more int64 columns per resident slab (the
  recomputed ``NextProgenitor`` and the next slab's ``FirstProgenitor``):
  **48 B per row, 144 B x n** over three slabs;
- pass 1 adds the inverse permutations of this and the next slab, the new
  forest, mass, the stored progenitor links and the promotion mask, about
  **49 B x n**, and the chain computation about **16 B x p** for every
  progenitor plus **110 B** per progenitor of a descendant with two or more
  (the census's vectorised insertion positions, before and after the cut);
- pass 2 adds the FoF chain construction (about 25 B x n) and, while
  verifying, the previous slab's ``Descendant`` and the structural masks
  (about 30 B x n).

Every whole-slab array is int64-indexed; nothing is narrowed to int32 but the
tree labels, whose ordinals the root enumeration bounds.
"""

#: The rewriter's own version, recorded in every manifest it writes.
REWRITER_VERSION = 1

#: The one source format the rewriter accepts and stamps.
SOURCE_FORMAT = "consistent_trees_ascii"

OPERATION_MIGRATE = "migrate"
OPERATION_NOOP = "noop"
OPERATION_CUT = "cut"
OPERATIONS = (OPERATION_MIGRATE, OPERATION_NOOP, OPERATION_CUT)

#: Files the rewriter writes beside the manifest, never inside ``dataset/``.
STATE_NAME = "rewrite_state.json"
SUMMARY_NAME = "rewrite_summary.json"
TREE_INVENTORY_NAME = "tree_inventory.npy"
CUT_PIECES_NAME = "cut_pieces.npy"
DATASET_DIR = "dataset"

__all__ = [
    "CUT_PIECES_NAME",
    "DATASET_DIR",
    "OPERATIONS",
    "OPERATION_CUT",
    "OPERATION_MIGRATE",
    "OPERATION_NOOP",
    "REWRITER_VERSION",
    "SOURCE_FORMAT",
    "STATE_NAME",
    "SUMMARY_NAME",
    "TREE_INVENTORY_NAME",
]
