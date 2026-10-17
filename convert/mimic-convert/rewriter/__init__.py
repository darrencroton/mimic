"""The horizontal-dataset rewriter: ``rewrite_trees.py``'s passes.

One tool reads a horizontal-HDF5 dataset of a Consistent-Trees ASCII catalogue
slab by slab and writes a version 3 dataset of the same halos under a forest
table (``convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md``, Version 3). Its
input is a version 3 dataset. Two operations are implemented here:

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
  output. The table need only satisfy its own invariants: one that splits a
  FoF group of the final snapshot is not refused, and the promotions it
  causes there are recorded in the summary's per-snapshot counts like any
  other.

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
   ``Descendant`` names the input's descendant row, that every untouched
   forest reproduces its payload, links and identity exactly, that every
   promoted halo is its own group's first halo, that each FoF group is one
   run of rows from its central, and that a walk from each descendant's
   ``FirstProgenitor`` through ``NextProgenitor`` reaches every progenitor of
   that descendant exactly once and nothing else.

**Output.** A converter-shaped workdir: ``manifest.json`` of the shape the
producer battery binds to, ``dataset/`` holding exactly the snapshot files
and ``forests.h5``, and beside the manifest ``tree_inventory.npy``,
``rewrite_state.json`` (the restart state), ``rewrite_summary.json`` and, for
a cut, ``cut_pieces.npy`` (every piece of every split forest).
``convert_trees.py validate --workdir <output>`` and ``report`` then run with
their binding and conservation checks applied.

**Memory.** Every phase's concurrent peak, for the no-op and the cut, is
stated once, in ``rewriter.inputs.MEMORY_FORMULAS``, which every summary
records and ``rewriter.inputs.planned_peak_bytes`` evaluates for an input (the
manifest's ``transpose_budget_bytes``). The cut's pass 1 is the run's peak.

Every whole-slab array is int64-indexed; nothing is narrowed to int32 but the
tree labels, whose ordinals the root enumeration bounds.
"""

#: The rewriter's own version, recorded in every manifest it writes.
REWRITER_VERSION = 2

#: The one source format the rewriter accepts and stamps.
SOURCE_FORMAT = "consistent_trees_ascii"

OPERATION_NOOP = "noop"
OPERATION_CUT = "cut"
OPERATIONS = (OPERATION_NOOP, OPERATION_CUT)

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
    "OPERATION_NOOP",
    "REWRITER_VERSION",
    "SOURCE_FORMAT",
    "STATE_NAME",
    "SUMMARY_NAME",
    "TREE_INVENTORY_NAME",
]
