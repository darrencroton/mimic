"""Slice 3 unit tests: the lossless L-Halo binary adapter
(scripts/convert/adapters/lhalo_binary.py).

**The oracle in this module is deliberately independent of the adapter.**
Fixtures are packed with a hand-written ``struct`` format and hand-declared
byte offsets, and are read back with ``struct.unpack_from`` -- never with
``column_schema``'s frozen layout, ``SourceLayout.numpy_dtype_spec()`` or any
of the adapter's own helpers. A test that built its expectation from the code
under test would confirm only that the code agrees with itself. The one place
the two meet is :class:`LayoutPinTests`, which exists precisely to fail loudly
if the shipped profile and this module's hand-written record ever diverge.

Fixtures are written into a temporary directory per test class rather than
committed as binaries. The committed fixtures under ``data/source_formats/``
serve the Slice 1 *inspection* route, which tolerates structures this adapter
must reject (a non-forward link is inspection data and a conversion error), so
sharing them would have meant either weakening this slice's gates or mutating
files another slice's tests depend on.

The structural rules asserted here were checked against all four shipped
L-Halo datasets -- mini-Millennium 8/8 files, micro-Uchuu 4/4, and four files
each of Millennium and mini-Uchuu, 75.4 M halos -- with zero violations before
they were made gates, so none of them rejects valid source data.
"""

import os
import shutil
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
from adapters import source_inventory as si  # noqa: E402
from adapters.base import NULL_LINK  # noqa: E402
from adapters.lhalo_binary import (  # noqa: E402
    INVENTORY_BYTES_PER_UNIT,
    TOPOLOGY_BYTES_PER_HALO,
    ConverterError,
    LHaloBinaryAdapter,
)

REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
PROFILE_DIR = os.path.join(REPO_ROOT, "scripts", "convert", "profiles")
MINI_MILLENNIUM = os.path.join(REPO_ROOT, "simulations", "mini-millennium")


# ==========================================================================
# Independent oracle: hand-written record layout, packer and reader
# ==========================================================================

#: (name, struct code, component count, hand-declared byte offset). Written
#: out by hand from struct RawHalo (src/include/generated/raw_halo_defs.h),
#: not derived from the profile this adapter loads.
ORACLE_LAYOUT = (
    ("Descendant", "i", 1, 0),
    ("FirstProgenitor", "i", 1, 4),
    ("NextProgenitor", "i", 1, 8),
    ("FirstHaloInFOFgroup", "i", 1, 12),
    ("NextHaloInFOFgroup", "i", 1, 16),
    ("Len", "i", 1, 20),
    ("M_Mean200", "f", 1, 24),
    ("M_Crit200", "f", 1, 28),
    ("M_TopHat", "f", 1, 32),
    ("Pos", "f", 3, 36),
    ("Vel", "f", 3, 48),
    ("VelDisp", "f", 1, 60),
    ("Vmax", "f", 1, 64),
    ("Spin", "f", 3, 68),
    ("MostBoundID", "q", 1, 80),
    ("SnapNum", "i", 1, 88),
    ("FileNr", "i", 1, 92),
    ("SubhaloIndex", "i", 1, 96),
    ("SubHalfMass", "f", 1, 100),
)

ORACLE_RECORD_BYTES = 104
_ORACLE_FORMAT = "".join(code * n for _name, code, n, _offset in ORACLE_LAYOUT)

#: Every field's default, so a fixture row declares only what it means to say.
#: A lone halo that is its own FoF central with no links is the simplest
#: structurally valid row.
_DEFAULTS = {
    "Descendant": -1,
    "FirstProgenitor": -1,
    "NextProgenitor": -1,
    "FirstHaloInFOFgroup": 0,
    "NextHaloInFOFgroup": -1,
    "Len": 1,
    "M_Mean200": 0.0,
    "M_Crit200": 0.0,
    "M_TopHat": 0.0,
    "Pos": (0.0, 0.0, 0.0),
    "Vel": (0.0, 0.0, 0.0),
    "VelDisp": 0.0,
    "Vmax": 0.0,
    "Spin": (0.0, 0.0, 0.0),
    "MostBoundID": 0,
    "SnapNum": 0,
    "FileNr": 0,
    "SubhaloIndex": 0,
    "SubHalfMass": 0.0,
}


def pack_record(row, byte_order="<"):
    """Pack one halo by hand, in the oracle's own declared field order."""
    values = dict(_DEFAULTS)
    values.update(row)
    flat = []
    for name, _code, n_components, _offset in ORACLE_LAYOUT:
        value = values[name]
        if n_components == 1:
            flat.append(value)
        else:
            flat.extend(value)
    packed = struct.pack(byte_order + _ORACLE_FORMAT, *flat)
    assert len(packed) == ORACLE_RECORD_BYTES, len(packed)
    return packed


def write_lhalo_file(path, trees, byte_order="<", trailing=b"", drop_records=0):
    """Write one L-Halo file: Ntrees, totNHalos, per-tree counts, records.

    ``trailing`` and ``drop_records`` exist so a test can produce a file whose
    payload disagrees with its own header in either direction.
    """
    counts = [len(tree) for tree in trees]
    records = [pack_record(row, byte_order) for tree in trees for row in tree]
    if drop_records:
        records = records[:-drop_records]
    with open(path, "wb") as handle:
        handle.write(struct.pack(byte_order + "i", len(trees)))
        handle.write(struct.pack(byte_order + "i", sum(counts)))
        for count in counts:
            handle.write(struct.pack(byte_order + "i", count))
        for record in records:
            handle.write(record)
        handle.write(trailing)
    return path


def read_counts_directly(path, byte_order="<"):
    """Read just the count header: ``(Ntrees, totNHalos, per-tree counts)``."""
    with open(path, "rb") as handle:
        ntrees, total = struct.unpack(byte_order + "ii", handle.read(8))
        counts = struct.unpack(byte_order + "{}i".format(ntrees), handle.read(4 * ntrees))
    return ntrees, total, list(counts)


def read_file_directly(path, byte_order="<", limit=None):
    """Read an L-Halo file back with ``struct``, returning trees of dicts.

    The independent extraction the plan's validation calls for: no numpy
    structured dtype, no converter helper, no schema. ``limit`` stops after
    that many trees and reads only their bytes, so a multi-gigabyte production
    file can be sampled without loading it.
    """
    ntrees, _total, counts = read_counts_directly(path, byte_order)
    if limit is not None:
        counts = counts[:limit]
    trees = []
    with open(path, "rb") as handle:
        handle.seek(8 + 4 * ntrees)
        for count in counts:
            blob = handle.read(count * ORACLE_RECORD_BYTES)
            tree = []
            for row_index in range(count):
                flat = struct.unpack_from(
                    byte_order + _ORACLE_FORMAT, blob, row_index * ORACLE_RECORD_BYTES
                )
                row = {}
                position = 0
                for name, _code, n_components, _field_offset in ORACLE_LAYOUT:
                    if n_components == 1:
                        row[name] = flat[position]
                    else:
                        row[name] = tuple(flat[position : position + n_components])
                    position += n_components
                tree.append(row)
            trees.append(tree)
    return trees


# ==========================================================================
# Fixture trees
# ==========================================================================

# TREE_A is the structurally interesting one. Snapshots 0, 1 and 2; two
# forward gaps (rows 3 and 6, span 2); one progenitor chain whose two members
# sit at *different* earlier snapshots (row 0's chain is row 2 at snapshot 1
# then row 3 at snapshot 0), which is the mixed-span case the plan names and
# which Slice 2's notes warn must not be read as "all links adjacent"; and a
# three-member FoF group at snapshot 0.
#
# Values are chosen to be hostile to a round trip: a negative mass sentinel, a
# signed zero, a subnormal, a value with no exact float32 form, duplicate and
# negative MostBoundIDs, and one MostBoundID above 2**53 where a float64
# detour would lose the low bit.
TREE_A = [
    {  # 0: snapshot 2, FoF central, no descendant
        "Descendant": -1,
        "FirstProgenitor": 2,
        "NextProgenitor": -1,
        "FirstHaloInFOFgroup": 0,
        "NextHaloInFOFgroup": 1,
        "SnapNum": 2,
        "Len": 500,
        "M_Crit200": 12.5,
        "M_Mean200": 13.5,
        "M_TopHat": 14.5,
        "Pos": (1.5, 2.25, 3.125),
        "Vel": (-10.5, 20.25, -30.125),
        "Spin": (0.5, -0.25, 0.125),
        "VelDisp": 100.5,
        "Vmax": 200.25,
        "MostBoundID": 9007199254740993,  # 2**53 + 1
        "FileNr": 7,
        "SubhaloIndex": 11,
        "SubHalfMass": 6.25,
    },
    {  # 1: snapshot 2, FoF satellite of row 0
        "Descendant": -1,
        "FirstProgenitor": 6,
        "NextProgenitor": -1,
        "FirstHaloInFOFgroup": 0,
        "NextHaloInFOFgroup": -1,
        "SnapNum": 2,
        "Len": 0,
        "M_Crit200": -1.0,  # negative mass sentinel: data, not a parse error
        "Pos": (0.0, -0.0, 1.0),  # signed zero must survive
        "MostBoundID": -42,  # signed catalog identifier
        "FileNr": 7,
    },
    {  # 2: snapshot 1, descends to row 0 with span 1
        "Descendant": 0,
        "FirstProgenitor": 4,
        "NextProgenitor": 3,
        "FirstHaloInFOFgroup": 2,
        "NextHaloInFOFgroup": -1,
        "SnapNum": 1,
        "Len": 300,
        "M_Crit200": 1.0 / 3.0,  # no exact float32 form
        "MostBoundID": 100,
        "FileNr": 7,
    },
    {  # 3: snapshot 0, descends to row 0 with span 2 -- a forward gap
        "Descendant": 0,
        "FirstHaloInFOFgroup": 3,
        "SnapNum": 0,
        "Len": 90,
        "M_Crit200": 1e-45,  # float32 subnormal
        "MostBoundID": 100,  # duplicate of row 2's
        "FileNr": 7,
    },
    {  # 4: snapshot 0, FoF central of a three-member group
        "Descendant": 2,
        "FirstHaloInFOFgroup": 4,
        "NextHaloInFOFgroup": 5,
        "NextProgenitor": 5,
        "SnapNum": 0,
        "Len": 150,
        "M_Crit200": 3.4028235e38,  # float32 max
        "MostBoundID": 101,
        "FileNr": 7,
    },
    {  # 5: snapshot 0, FoF satellite, sibling of row 4
        "Descendant": 2,
        "FirstHaloInFOFgroup": 4,
        "NextHaloInFOFgroup": 6,
        "SnapNum": 0,
        "Len": 120,
        "MostBoundID": 101,  # duplicate of row 4's
        "FileNr": 7,
    },
    {  # 6: snapshot 0, FoF satellite, descends to row 1 with span 2 -- a gap
        "Descendant": 1,
        "FirstHaloInFOFgroup": 4,
        "NextHaloInFOFgroup": -1,
        "SnapNum": 0,
        "Len": 60,
        "MostBoundID": -1,
        "FileNr": 7,
    },
]

# TREE_B: a two-halo tree, so a file carries more than one unit.
TREE_B = [
    {
        "Descendant": -1,
        "FirstProgenitor": 1,
        "FirstHaloInFOFgroup": 0,
        "SnapNum": 1,
        "Len": 20,
        "MostBoundID": 900,
    },
    {
        "Descendant": 0,
        "FirstHaloInFOFgroup": 1,
        "SnapNum": 0,
        "Len": 10,
        "MostBoundID": 901,
    },
]

#: Every source-key edge TREE_A declares, hand-read off the table above as
#: (row, link name, target row). Nothing here is computed from the adapter.
TREE_A_EDGES = (
    (0, "FirstProgenitor", 2),
    (0, "FirstHaloInFOFgroup", 0),
    (0, "NextHaloInFOFgroup", 1),
    (1, "FirstProgenitor", 6),
    (1, "FirstHaloInFOFgroup", 0),
    (2, "Descendant", 0),
    (2, "FirstProgenitor", 4),
    (2, "NextProgenitor", 3),
    (2, "FirstHaloInFOFgroup", 2),
    (3, "Descendant", 0),
    (3, "FirstHaloInFOFgroup", 3),
    (4, "Descendant", 2),
    (4, "NextProgenitor", 5),
    (4, "FirstHaloInFOFgroup", 4),
    (4, "NextHaloInFOFgroup", 5),
    (5, "Descendant", 2),
    (5, "FirstHaloInFOFgroup", 4),
    (5, "NextHaloInFOFgroup", 6),
    (6, "Descendant", 1),
    (6, "FirstHaloInFOFgroup", 4),
)

LINK_NAMES = (
    "Descendant",
    "FirstProgenitor",
    "NextProgenitor",
    "FirstHaloInFOFgroup",
    "NextHaloInFOFgroup",
)


def linear_tree(n_halos):
    """A valid ``n_halos``-row tree: one halo per snapshot, one chain.

    Row ``i`` sits at snapshot ``n - 1 - i`` and descends to row ``i - 1``, so
    the tree is a single unbranched line with every structural rule satisfied
    and every halo its own FoF central. Used where a test needs a *size* rather
    than a shape.
    """
    return [
        {
            "Descendant": index - 1 if index else -1,
            "FirstProgenitor": index + 1 if index + 1 < n_halos else -1,
            "NextProgenitor": -1,
            "FirstHaloInFOFgroup": index,
            "NextHaloInFOFgroup": -1,
            "SnapNum": n_halos - 1 - index,
            "Len": index + 1,
            "MostBoundID": 1000 + index,
        }
        for index in range(n_halos)
    ]


def default_schema():
    """The shipped default profile, frozen against mini-Millennium's record."""
    column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml"))
    properties = cs.load_source_properties(os.path.join(MINI_MILLENNIUM, "halo_properties.yaml"))
    return cs.build_schema(column_map, properties)


def extras_schema():
    """The shipped worked example, which selects every remaining field."""
    column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary_extras_example.yaml"))
    properties = cs.load_source_properties(os.path.join(MINI_MILLENNIUM, "halo_properties.yaml"))
    return cs.build_schema(column_map, properties)


def big_endian_schema():
    """The default profile with its declared byte order flipped."""
    column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml"))
    raw = {
        "schema_version": column_map.schema_version,
        "source_format": column_map.source_format,
        "required_columns": {role: list(aliases) for role, aliases in column_map.required_columns},
        "extra_fields": [],
        "binary_layout": {
            "byte_order": "big",
            "itemsize": column_map.binary_layout.itemsize,
            "offsets": dict(column_map.binary_layout.offsets),
        },
    }
    properties = cs.load_source_properties(os.path.join(MINI_MILLENNIUM, "halo_properties.yaml"))
    return cs.build_schema(cs.parse_column_map(raw, origin="<big-endian test>"), properties)


def collect(adapter, max_rows):
    """Drain an adapter into one dict of concatenated columns, plus the sizes.

    Returns ``(columns, batch_sizes)`` where ``columns`` is
    ``group -> name -> array`` over every emitted row in emission order.
    """
    groups = {}
    sizes = []
    for batch in adapter.iter_batches(max_rows):
        sizes.append(batch.n_rows)
        for group, mapping in (
            ("identity", batch.identity),
            ("coordinates", batch.coordinates),
            ("links", batch.links),
            ("payload", batch.payload),
            ("extras", batch.extras),
        ):
            for name, values in mapping.items():
                groups.setdefault(group, {}).setdefault(name, []).append(np.asarray(values))
    return (
        {
            group: {name: np.concatenate(parts) for name, parts in mapping.items()}
            for group, mapping in groups.items()
        },
        sizes,
    )


class FixtureCase(unittest.TestCase):
    """Base class owning a temporary source directory."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="mimic-lhalo-")
        self.addCleanup(shutil.rmtree, self.tmpdir, True)

    def path(self, name):
        return os.path.join(self.tmpdir, name)

    def write(self, name, trees, **kwargs):
        return write_lhalo_file(self.path(name), trees, **kwargs)

    def adapter(self, sources, schema=None, **kwargs):
        return LHaloBinaryAdapter(schema or default_schema(), sources, **kwargs)


# ==========================================================================
# The pin between the oracle and the shipped profile
# ==========================================================================


class LayoutPinTests(unittest.TestCase):
    def test_oracle_layout_matches_the_shipped_profile(self):
        """The oracle is hand-written; this is where it is held to account.

        If struct RawHalo, the package ``halo_properties.yaml`` or the shipped
        profile ever changes, this test fails first and names the divergence,
        rather than every value test quietly comparing two copies of a stale
        layout.
        """
        layout = default_schema().source_layout
        self.assertEqual(layout.itemsize, ORACLE_RECORD_BYTES)
        self.assertEqual(
            [(entry.name, entry.offset) for entry in layout.entries],
            [(name, offset) for name, _code, _n, offset in ORACLE_LAYOUT],
        )
        widths = {"i": 4, "f": 4, "q": 8}
        for (name, code, n_components, offset), entry in zip(ORACLE_LAYOUT, layout.entries):
            self.assertEqual(entry.itemsize, widths[code] * n_components, name)
            self.assertEqual(entry.n_components, n_components, name)
            self.assertEqual(entry.offset, offset, name)

    def test_struct_format_packs_without_alignment_padding(self):
        self.assertEqual(struct.calcsize("<" + _ORACLE_FORMAT), ORACLE_RECORD_BYTES)

    def test_every_shipped_binary_package_freezes_the_same_layout(self):
        """One adapter covers mini/micro-Uchuu and both Millennium packages.

        The plan's input criterion names all four, and they are only one
        adapter's job because all four declare the identical ordered record.
        Checked against each package's own ``halo_properties.yaml`` rather
        than assumed from mini-Millennium's.
        """
        reference = None
        for package in ("mini-millennium", "millennium", "micro-uchuu", "mini-uchuu"):
            properties_path = os.path.join(
                REPO_ROOT, "simulations", package, "halo_properties.yaml"
            )
            self.assertTrue(os.path.exists(properties_path), package)
            column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml"))
            layout = cs.build_schema(
                column_map, cs.load_source_properties(properties_path)
            ).source_layout
            frozen = (
                layout.byte_order,
                layout.itemsize,
                tuple((e.name, e.type, e.offset, e.itemsize) for e in layout.entries),
            )
            self.assertEqual(layout.itemsize, ORACLE_RECORD_BYTES, package)
            if reference is None:
                reference = frozen
            else:
                self.assertEqual(frozen, reference, package)


# ==========================================================================
# Inventory, file selection and identity
# ==========================================================================


class InventoryTests(FixtureCase):
    def test_units_counts_and_prefix_ids_span_files(self):
        self.write("trees.0", [TREE_A, TREE_B])
        self.write("trees.1", [TREE_B])
        inventory = self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))]).inventory()

        self.assertEqual(
            [(u.source_file_ordinal, u.unit_ordinal, u.n_halos) for u in inventory.units],
            [(0, 0, 7), (0, 1, 2), (1, 0, 2)],
        )
        self.assertEqual(inventory.total_halos, 11)
        self.assertEqual(inventory.base_id(0, 0), 1)
        self.assertEqual(inventory.base_id(0, 1), 8)
        self.assertEqual(inventory.base_id(1, 0), 10)

    def test_file_ordinals_are_the_sources_own_numbers_not_positions(self):
        """A conversion of files 4 and 5 records 4 and 5, not 0 and 1."""
        self.write("trees.4", [TREE_B])
        self.write("trees.5", [TREE_B])
        adapter = self.adapter([(4, self.path("trees.4")), (5, self.path("trees.5"))])
        columns, _sizes = collect(adapter, 16)
        self.assertEqual(
            sorted(set(columns["coordinates"]["source_file_ordinal"].tolist())), [4, 5]
        )

    def test_missing_requested_file_fails_rather_than_narrowing(self):
        self.write("trees.0", [TREE_B])
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))])
        self.assertIn("is missing", str(caught.exception))

    def test_directory_in_place_of_a_source_file_fails(self):
        os.mkdir(self.path("trees.0"))
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))])
        self.assertIn("not a regular file", str(caught.exception))

    def test_out_of_order_or_duplicate_ordinals_fail(self):
        self.write("trees.0", [TREE_B])
        self.write("trees.1", [TREE_B])
        for ordinals in ((1, 0), (0, 0)):
            with self.assertRaises(ConverterError) as caught:
                self.adapter(
                    [
                        (ordinals[0], self.path("trees.0")),
                        (ordinals[1], self.path("trees.1")),
                    ]
                )
            self.assertIn("ascending", str(caught.exception))

    def test_negative_ordinal_fails(self):
        self.write("trees.0", [TREE_B])
        with self.assertRaises(ConverterError):
            self.adapter([(-1, self.path("trees.0"))])

    def test_empty_source_list_fails(self):
        with self.assertRaises(ConverterError) as caught:
            self.adapter([])
        self.assertIn("no source files requested", str(caught.exception))

    def test_malformed_source_entry_fails(self):
        with self.assertRaises(ConverterError) as caught:
            self.adapter([self.path("trees.0")])
        self.assertIn("(source_file_ordinal, path) pair", str(caught.exception))

    def test_zero_tree_file_yields_an_empty_inventory_and_no_batches(self):
        self.write("trees.0", [])
        adapter = self.adapter([(0, self.path("trees.0"))])
        self.assertEqual(adapter.inventory().units, ())
        self.assertEqual(adapter.inventory().total_halos, 0)
        self.assertEqual(list(adapter.iter_batches(8)), [])

    def test_zero_halo_tree_is_an_inventory_unit_with_no_rows(self):
        self.write("trees.0", [TREE_B, [], TREE_B])
        adapter = self.adapter([(0, self.path("trees.0"))])
        inventory = adapter.inventory()
        self.assertEqual([u.n_halos for u in inventory.units], [2, 0, 2])
        # The empty unit consumes no id range, so the third unit starts where
        # the first one ended.
        self.assertEqual(inventory.base_id(0, 1), 3)
        self.assertEqual(inventory.base_id(0, 2), 3)
        columns, _sizes = collect(adapter, 8)
        self.assertEqual(columns["identity"]["SourceHaloID"].tolist(), [1, 2, 3, 4])
        self.assertEqual(columns["coordinates"]["unit_ordinal"].tolist(), [0, 0, 2, 2])

    def test_forest_index_is_the_file_prefix_tree_number(self):
        """Matches the vertical driver's GlobalForestOffset + unit exactly.

        ``build_partition_file_offsets()`` in src/core/vertical_driver.c makes
        the per-file prefix the cumulative tree count over preceding files, and
        ``make_unique_galaxy_id()`` in src/core/build_model.c adds the unit
        index to it.
        """
        self.write("trees.0", [TREE_B, TREE_B, TREE_B])  # 3 trees
        self.write("trees.1", [TREE_B, TREE_B])  # 2 trees
        adapter = self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))])
        columns, _sizes = collect(adapter, 64)
        # Two rows per tree, five trees, so the prefix numbers run 0..4.
        self.assertEqual(
            columns["identity"]["ForestIndex"].tolist(), [0, 0, 1, 1, 2, 2, 3, 3, 4, 4]
        )
        self.assertEqual(columns["identity"]["HaloRankInForest"].tolist(), [0, 1] * 5)

    def test_source_halo_id_is_not_the_catalog_identifier(self):
        self.write("trees.0", [TREE_A])
        columns, _sizes = collect(self.adapter([(0, self.path("trees.0"))]), 16)
        ids = columns["identity"]["SourceHaloID"]
        most_bound = columns["payload"]["MostBoundID"]
        self.assertEqual(ids.tolist(), list(range(1, 8)))
        self.assertTrue(bool((ids != most_bound).all()))
        # The source's own identifiers are duplicated and negative here; the
        # converter's key is neither.
        self.assertLess(int(most_bound.min()), 0)
        self.assertLess(len(set(most_bound.tolist())), most_bound.size)


# ==========================================================================
# Header, layout and byte-order validation
# ==========================================================================


class HeaderAndLayoutTests(FixtureCase):
    def test_truncated_payload_fails(self):
        self.write("trees.0", [TREE_A], drop_records=1)
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))]).inventory()
        self.assertIn("truncated or trailing data", str(caught.exception))

    def test_trailing_bytes_fail(self):
        self.write("trees.0", [TREE_A], trailing=b"\x00" * 16)
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))]).inventory()
        self.assertIn("truncated or trailing data", str(caught.exception))

    def test_truncated_header_fails(self):
        with open(self.path("trees.0"), "wb") as handle:
            handle.write(struct.pack("<i", 2))
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))]).inventory()
        self.assertIn("truncated header", str(caught.exception))

    def test_count_total_disagreeing_with_the_table_fails(self):
        with open(self.path("trees.0"), "wb") as handle:
            handle.write(struct.pack("<ii", 2, 99))
            handle.write(struct.pack("<ii", 7, 2))
            for tree in (TREE_A, TREE_B):
                for row in tree:
                    handle.write(pack_record(row))
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))]).inventory()
        self.assertIn("bad count total", str(caught.exception))

    def test_little_endian_file_read_through_a_big_endian_profile_fails(self):
        """Byte order is a property of the file, and a mismatch must not read.

        The header's own counts are the first thing to go incoherent, so this
        fails before any record is interpreted.
        """
        self.write("trees.0", [TREE_A])
        with self.assertRaises(ConverterError):
            self.adapter([(0, self.path("trees.0"))], schema=big_endian_schema()).inventory()

    def test_big_endian_source_produces_identical_native_output(self):
        self.write("little.0", [TREE_A])
        self.write("big.0", [TREE_A], byte_order=">")
        little, _sizes = collect(self.adapter([(0, self.path("little.0"))]), 16)
        big, _sizes = collect(
            self.adapter([(0, self.path("big.0"))], schema=big_endian_schema()), 16
        )
        for group in ("identity", "coordinates", "links", "payload"):
            self.assertEqual(sorted(little[group]), sorted(big[group]))
            for name, values in little[group].items():
                other = big[group][name]
                # Native byte order on both sides, and bit-identical values.
                self.assertEqual(other.dtype.byteorder, "=", name)
                self.assertEqual(values.tobytes(), other.tobytes(), name)

    def test_a_profile_whose_itemsize_disagrees_with_the_file_fails(self):
        """The stride is validated against the file's real length.

        A record declared one byte too wide makes the header-implied size
        disagree with the file, which must fail rather than misread every
        record after the first.
        """
        self.write("trees.0", [TREE_A])
        column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml"))
        raw = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": {
                role: list(aliases) for role, aliases in column_map.required_columns
            },
            "extra_fields": [],
            "binary_layout": {
                "byte_order": "little",
                "itemsize": ORACLE_RECORD_BYTES + 8,
                "offsets": dict(column_map.binary_layout.offsets),
            },
        }
        properties = cs.load_source_properties(
            os.path.join(MINI_MILLENNIUM, "halo_properties.yaml")
        )
        schema = cs.build_schema(cs.parse_column_map(raw, origin="<wide test>"), properties)
        with self.assertRaises(ConverterError) as caught:
            self.adapter([(0, self.path("trees.0"))], schema=schema).inventory()
        self.assertIn("truncated or trailing data", str(caught.exception))

    def test_unselected_fields_remain_part_of_the_declared_stride(self):
        """Selecting nothing extra does not shrink the record.

        The default profile selects neither ``M_Mean200`` nor ``FileNr``, yet
        both keep their bytes in the frozen layout, and dropping one from
        ``offsets`` is rejected outright.
        """
        layout = default_schema().source_layout
        unselected = [entry.name for entry in layout.entries if not entry.selected]
        self.assertEqual(
            sorted(unselected), ["FileNr", "M_Mean200", "M_TopHat", "SubHalfMass", "SubhaloIndex"]
        )
        self.assertEqual(sum(entry.itemsize for entry in layout.entries), ORACLE_RECORD_BYTES)

        column_map = cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml"))
        offsets = dict(column_map.binary_layout.offsets)
        del offsets["M_Mean200"]
        raw = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": {
                role: list(aliases) for role, aliases in column_map.required_columns
            },
            "extra_fields": [],
            "binary_layout": {"byte_order": "little", "itemsize": 104, "offsets": offsets},
        }
        properties = cs.load_source_properties(
            os.path.join(MINI_MILLENNIUM, "halo_properties.yaml")
        )
        with self.assertRaises(ConverterError) as caught:
            cs.build_schema(cs.parse_column_map(raw, origin="<gap test>"), properties)
        self.assertIn("does not cover source field", str(caught.exception))


# ==========================================================================
# Value and link preservation, against the independent extraction
# ==========================================================================


class PreservationTests(FixtureCase):
    def setUp(self):
        super().setUp()
        self.write("trees.0", [TREE_A, TREE_B])
        self.source = read_file_directly(self.path("trees.0"))
        self.columns, _sizes = collect(self.adapter([(0, self.path("trees.0"))]), 16)

    def flat_source(self):
        return [row for tree in self.source for row in tree]

    def test_every_payload_value_matches_the_direct_binary_read(self):
        rows = self.flat_source()
        payload = self.columns["payload"]
        for index, row in enumerate(rows):
            for name in ("Len", "SnapNum", "MostBoundID"):
                self.assertEqual(int(payload[name][index]), row[name], (index, name))
            for name in ("M_Crit200", "VelDisp", "Vmax"):
                self.assertEqual(
                    np.float32(payload[name][index]).tobytes(),
                    struct.pack("<f", row[name]),
                    (index, name),
                )
            for name in ("Pos", "Vel", "Spin"):
                self.assertEqual(
                    np.asarray(payload[name][index], dtype="<f4").tobytes(),
                    struct.pack("<3f", *row[name]),
                    (index, name),
                )

    def test_declared_payload_dtypes_are_the_native_source_widths(self):
        payload = self.columns["payload"]
        self.assertEqual(payload["M_Crit200"].dtype, np.dtype("float32"))
        self.assertEqual(payload["Pos"].dtype, np.dtype("float32"))
        self.assertEqual(payload["Pos"].shape, (9, 3))
        self.assertEqual(payload["MostBoundID"].dtype, np.dtype("int64"))
        self.assertEqual(payload["Len"].dtype, np.dtype("int32"))
        self.assertEqual(payload["SnapNum"].dtype, np.dtype("int32"))

    def test_mass_is_not_round_tripped_through_another_unit(self):
        """float32 in 1e10 Msun/h, untouched.

        ``1/3`` and ``1e-45`` have no exact representation in the units a
        round trip would pass through, so a conversion to Msun/h and back
        would be visible here as a changed bit pattern.
        """
        masses = self.columns["payload"]["M_Crit200"]
        self.assertEqual(masses[2].tobytes(), struct.pack("<f", 1.0 / 3.0))
        self.assertEqual(masses[3].tobytes(), struct.pack("<f", 1e-45))
        self.assertEqual(masses[4].tobytes(), struct.pack("<f", 3.4028235e38))

    def test_negative_mass_sentinel_survives_as_data(self):
        self.assertEqual(float(self.columns["payload"]["M_Crit200"][1]), -1.0)

    def test_signed_zero_is_preserved(self):
        position = self.columns["payload"]["Pos"][1]
        self.assertEqual(float(position[0]), 0.0)
        self.assertEqual(float(position[1]), 0.0)
        self.assertFalse(bool(np.signbit(position[0])))
        self.assertTrue(bool(np.signbit(position[1])))

    def test_mostboundid_keeps_duplicates_negatives_and_bits_above_2_53(self):
        most_bound = self.columns["payload"]["MostBoundID"]
        self.assertEqual(int(most_bound[0]), 2**53 + 1)
        self.assertEqual(int(most_bound[1]), -42)
        self.assertEqual(int(most_bound[2]), int(most_bound[3]))
        self.assertEqual(int(most_bound[4]), int(most_bound[5]))

    def test_len_and_spin_are_taken_from_the_source_not_derived(self):
        """Len is supplied, and Spin is not renormalised.

        Row 1 carries ``Len == 0`` beside ``M_Crit200 == -1.0``: any attempt to
        derive Len from mass, or to divide Spin by mass the way the
        Consistent-Trees producer does, would be visible immediately.
        """
        self.assertEqual(int(self.columns["payload"]["Len"][1]), 0)
        self.assertEqual(
            np.asarray(self.columns["payload"]["Spin"][0], dtype="<f4").tobytes(),
            struct.pack("<3f", 0.5, -0.25, 0.125),
        )

    def test_row_order_is_the_source_order(self):
        coordinates = self.columns["coordinates"]
        self.assertEqual(coordinates["row_ordinal"].tolist(), [0, 1, 2, 3, 4, 5, 6, 0, 1])
        self.assertEqual(coordinates["unit_ordinal"].tolist(), [0] * 7 + [1] * 2)
        self.assertEqual(
            self.columns["identity"]["HaloRankInForest"].tolist(), [0, 1, 2, 3, 4, 5, 6, 0, 1]
        )

    def test_all_five_chains_survive_as_exact_source_key_edges(self):
        """Hand-declared edges, compared as target SourceHaloIDs.

        TREE_A starts at SourceHaloID 1, so a stored within-tree row index
        ``r`` must appear as ``r + 1``. Every link the table does not list must
        be null.
        """
        links = self.columns["links"]
        expected = {(row, name): target + 1 for row, name, target in TREE_A_EDGES}
        for row in range(7):
            for name in LINK_NAMES:
                actual = int(links[name][row])
                self.assertEqual(actual, expected.get((row, name), NULL_LINK), (row, name))

    def test_forward_gaps_survive_with_their_exact_targets(self):
        """Rows 3 and 6 skip a snapshot; both edges are kept, not dropped."""
        links = self.columns["links"]
        snapshots = self.columns["payload"]["SnapNum"]
        ids = self.columns["identity"]["SourceHaloID"]
        by_id = {int(value): index for index, value in enumerate(ids)}
        gaps = []
        for row in (3, 6):
            target = by_id[int(links["Descendant"][row])]
            gaps.append(int(snapshots[target]) - int(snapshots[row]))
        self.assertEqual(gaps, [2, 2])
        self.assertEqual(int(links["Descendant"][3]), 1)  # row 0 of this tree
        self.assertEqual(int(links["Descendant"][6]), 2)  # row 1 of this tree

    def test_a_progenitor_chain_may_mix_spans(self):
        """Row 0's chain is row 2 (span 1) then row 3 (span 2).

        Slice 2's note that NextProgenitor is descendant-relative rather than
        owner-relative is exactly this: the two chain members share a
        descendant but sit at different snapshots, and nothing here may assume
        adjacency.
        """
        links = self.columns["links"]
        snapshots = self.columns["payload"]["SnapNum"]
        self.assertEqual(int(links["FirstProgenitor"][0]), 3)  # row 2
        self.assertEqual(int(links["NextProgenitor"][2]), 4)  # row 3
        self.assertEqual(int(snapshots[2]), 1)
        self.assertEqual(int(snapshots[3]), 0)
        self.assertEqual(int(links["Descendant"][2]), int(links["Descendant"][3]))

    def test_links_across_a_tree_boundary_stay_inside_their_own_tree(self):
        """TREE_B's links resolve to TREE_B's id range, not TREE_A's."""
        links = self.columns["links"]
        self.assertEqual(int(links["FirstProgenitor"][7]), 9)
        self.assertEqual(int(links["Descendant"][8]), 8)


# ==========================================================================
# Declaratively selected extras
# ==========================================================================


class ExtraFieldTests(FixtureCase):
    def setUp(self):
        super().setUp()
        self.write("trees.0", [TREE_A])
        self.source = read_file_directly(self.path("trees.0"))[0]
        self.columns, _sizes = collect(
            self.adapter([(0, self.path("trees.0"))], schema=extras_schema()), 16
        )

    def test_selected_extras_match_the_direct_binary_read(self):
        extras = self.columns["extras"]
        self.assertEqual(
            sorted(extras),
            ["M_Mean200", "M_TopHat", "PosX", "SourceFileNr", "SubHalfMass", "SubhaloIndex"],
        )
        for index, row in enumerate(self.source):
            self.assertEqual(int(extras["SourceFileNr"][index]), row["FileNr"], index)
            self.assertEqual(int(extras["SubhaloIndex"][index]), row["SubhaloIndex"], index)
            for output, source in (
                ("M_Mean200", "M_Mean200"),
                ("M_TopHat", "M_TopHat"),
                ("SubHalfMass", "SubHalfMass"),
            ):
                self.assertEqual(
                    np.float32(extras[output][index]).tobytes(),
                    struct.pack("<f", row[source]),
                    (index, output),
                )

    def test_a_vector_component_extra_selects_that_component_only(self):
        extras = self.columns["extras"]
        for index, row in enumerate(self.source):
            self.assertEqual(
                np.float32(extras["PosX"][index]).tobytes(),
                struct.pack("<f", row["Pos"][0]),
                index,
            )
        self.assertEqual(extras["PosX"].shape, (7,))

    def test_extras_keep_their_declared_precision(self):
        extras = self.columns["extras"]
        self.assertEqual(extras["M_Mean200"].dtype, np.dtype("float32"))
        self.assertEqual(extras["SourceFileNr"].dtype, np.dtype("int32"))
        self.assertEqual(extras["PosX"].dtype, np.dtype("float32"))

    def test_selecting_extras_does_not_change_the_payload(self):
        plain, _sizes = collect(self.adapter([(0, self.path("trees.0"))]), 16)
        for name, values in plain["payload"].items():
            self.assertEqual(values.tobytes(), self.columns["payload"][name].tobytes(), name)


# ==========================================================================
# Chunking
# ==========================================================================


class ChunkingTests(FixtureCase):
    def test_a_tree_larger_than_the_chunk_is_split_across_batches(self):
        self.write("trees.0", [TREE_A])
        adapter = self.adapter([(0, self.path("trees.0"))])
        columns, sizes = collect(adapter, 2)
        self.assertEqual(sizes, [2, 2, 2, 1])
        self.assertTrue(all(size <= 2 for size in sizes))
        self.assertEqual(columns["identity"]["SourceHaloID"].tolist(), list(range(1, 8)))

    def test_batches_pack_across_tree_and_file_boundaries(self):
        self.write("trees.0", [TREE_B, TREE_B])
        self.write("trees.1", [TREE_B])
        adapter = self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))])
        _columns, sizes = collect(adapter, 4)
        # Six rows in three two-row trees across two files: full batches, not
        # one batch per tree.
        self.assertEqual(sizes, [4, 2])

    def test_chunking_does_not_change_the_emitted_rows(self):
        self.write("trees.0", [TREE_A, TREE_B])
        self.write("trees.1", [TREE_A])
        reference, _sizes = collect(
            self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))]), 1024
        )
        for max_rows in (1, 2, 3, 5, 7, 16):
            columns, sizes = collect(
                self.adapter([(0, self.path("trees.0")), (1, self.path("trees.1"))]), max_rows
            )
            self.assertTrue(all(size <= max_rows for size in sizes), max_rows)
            for group, mapping in reference.items():
                for name, values in mapping.items():
                    self.assertEqual(
                        values.tobytes(), columns[group][name].tobytes(), (max_rows, group, name)
                    )

    def test_max_rows_must_be_positive(self):
        self.write("trees.0", [TREE_B])
        with self.assertRaises(ConverterError):
            list(self.adapter([(0, self.path("trees.0"))]).iter_batches(0))


# ==========================================================================
# Budgets
# ==========================================================================


class BudgetTests(FixtureCase):
    def test_an_over_budget_inventory_fails_before_it_is_built(self):
        self.write("trees.0", [TREE_B, TREE_B, TREE_B])
        with self.assertRaises(ConverterError) as caught:
            self.adapter(
                [(0, self.path("trees.0"))], memory_budget_bytes=INVENTORY_BYTES_PER_UNIT
            ).inventory()
        self.assertIn("memory budget", str(caught.exception))

    def test_an_over_budget_tree_topology_fails_before_allocation(self):
        """A budget that admits the inventory but not the tree's topology.

        One unit costs ``INVENTORY_BYTES_PER_UNIT``; a 20-halo tree's topology
        costs ``20 * TOPOLOGY_BYTES_PER_HALO``, which is the larger of the two,
        so a budget between them isolates the topology check.
        """
        self.write("trees.0", [linear_tree(20)])
        budget = 10 * TOPOLOGY_BYTES_PER_HALO
        self.assertGreater(budget, INVENTORY_BYTES_PER_UNIT)
        adapter = self.adapter([(0, self.path("trees.0"))], memory_budget_bytes=budget)
        with self.assertRaises(ConverterError) as caught:
            list(adapter.iter_batches(2))
        self.assertIn("topology validation for 20 halos", str(caught.exception))

    def test_a_non_positive_budget_is_rejected(self):
        self.write("trees.0", [TREE_B])
        with self.assertRaises(ConverterError):
            self.adapter([(0, self.path("trees.0"))], memory_budget_bytes=0)


# ==========================================================================
# Structural rejection
# ==========================================================================


class RejectionTests(FixtureCase):
    """Every rule here was first confirmed to hold on all four real datasets.

    Each case mutates one field of an otherwise valid tree, so the failure is
    attributable to that field and nothing else.
    """

    def reject(self, tree, fragment, **adapter_kwargs):
        self.write("trees.0", [tree])
        adapter = self.adapter([(0, self.path("trees.0"))], **adapter_kwargs)
        with self.assertRaises(ConverterError) as caught:
            list(adapter.iter_batches(4))
        self.assertIn(fragment, str(caught.exception))
        return str(caught.exception)

    @staticmethod
    def mutate(changes=None):
        """A copy of TREE_A with ``{row_index: {field: value}}`` applied."""
        tree = [dict(row) for row in TREE_A]
        for index, updates in (changes or {}).items():
            tree[index].update(updates)
        return tree

    # ---- link ranges ----------------------------------------------------

    def test_out_of_tree_link_fails(self):
        self.reject(self.mutate({2: {"Descendant": 99}}), "outside this 7-halo tree")

    def test_link_below_the_null_sentinel_fails(self):
        self.reject(self.mutate({2: {"NextProgenitor": -2}}), "only -1 is the null sentinel")

    def test_null_fof_central_fails(self):
        self.reject(self.mutate({1: {"FirstHaloInFOFgroup": -1}}), "it is never null")

    # ---- snapshots ------------------------------------------------------

    def test_negative_snapshot_fails(self):
        self.reject(self.mutate({6: {"SnapNum": -1}}), "SnapNum is -1, which is negative")

    def test_snapshot_beyond_the_a_list_fails(self):
        self.reject(
            self.mutate(),
            "outside the a_list's range [0, 1]",
            max_snapshot=1,
        )

    # ---- descendants ----------------------------------------------------

    def test_same_snapshot_descendant_fails(self):
        """Span 0 is not a gap. Built fresh, so no other rule can fire first."""
        tree = [
            {"Descendant": 1, "FirstHaloInFOFgroup": 0, "SnapNum": 1},
            {"Descendant": -1, "FirstProgenitor": 0, "FirstHaloInFOFgroup": 1, "SnapNum": 1},
        ]
        self.reject(tree, "not forward of snapshot")

    def test_backwards_descendant_fails(self):
        tree = [
            {"Descendant": 1, "FirstHaloInFOFgroup": 0, "SnapNum": 2},
            {"Descendant": -1, "FirstProgenitor": 0, "FirstHaloInFOFgroup": 1, "SnapNum": 1},
        ]
        self.reject(tree, "not forward of snapshot")

    def test_a_forward_gap_is_not_rejected(self):
        """The counterpart to the two above: span 2 is legal and converts."""
        tree = [
            {"Descendant": 1, "FirstHaloInFOFgroup": 0, "SnapNum": 0},
            {"Descendant": -1, "FirstProgenitor": 0, "FirstHaloInFOFgroup": 1, "SnapNum": 2},
        ]
        self.write("trees.0", [tree])
        columns, _sizes = collect(self.adapter([(0, self.path("trees.0"))]), 4)
        self.assertEqual(columns["links"]["Descendant"].tolist(), [2, NULL_LINK])

    # ---- progenitor chains ----------------------------------------------

    def test_a_forward_pointing_first_progenitor_fails(self):
        """C3's "FirstProgenitor points backwards", enforced transitively.

        There is no tree where a forward-pointing FirstProgenitor is the
        *only* defect: reciprocity plus the descendant-forward rule already
        imply the backwards direction, so a dedicated branch for it would be
        unreachable. What matters is that such a tree is refused, which it is
        -- here by the reciprocity rule.
        """
        self.reject(self.mutate({0: {"FirstProgenitor": 1}}), "round trip is inconsistent")

    def test_broken_progenitor_round_trip_fails(self):
        self.reject(self.mutate({1: {"FirstProgenitor": 5}}), "round trip is inconsistent")

    def test_sibling_naming_a_different_descendant_fails(self):
        self.reject(self.mutate({2: {"NextProgenitor": 6}}), "siblings must name the same")

    def test_sibling_on_a_halo_with_no_descendant_fails(self):
        self.reject(self.mutate({0: {"NextProgenitor": 1}}), "no chain for it to belong to")

    def test_missing_chain_head_fails(self):
        self.reject(self.mutate({1: {"FirstProgenitor": -1}}), "but FirstProgenitor is -1")

    def test_a_halo_named_twice_by_progenitor_pointers_fails(self):
        """Row 3 is already row 0's chain tail; the head names it again.

        Reciprocity still holds -- row 3 does descend to row 0 -- and every
        chain head still has progenitors, so only the in-degree count sees
        that row 3 is named twice and row 2 not at all.
        """
        self.reject(self.mutate({0: {"FirstProgenitor": 3}}), "double-count")

    def test_a_progenitor_cycle_fails(self):
        """In-degree stays exactly one, so only reachability catches this.

        Rows 4, 5 and 6 all descend to row 2. Row 2's chain head names row 4
        and stops there, while rows 5 and 6 point at each other. Every halo
        with a descendant still has an in-degree of exactly one, and every
        chain head still has progenitors -- the corruption is a two-cycle
        disjoint from the head, which only the reachability count sees.
        """
        tree = self.mutate(
            {
                1: {"FirstProgenitor": -1},
                4: {"NextProgenitor": -1},
                5: {"NextProgenitor": 6},
                6: {"Descendant": 2, "NextProgenitor": 5},
            }
        )
        message = self.reject(tree, "form a cycle")
        self.assertIn("NextProgenitor chains reach", message)

    # ---- FoF membership -------------------------------------------------

    def test_cross_snapshot_fof_membership_fails(self):
        self.reject(
            self.mutate({2: {"FirstHaloInFOFgroup": 0}}),
            "FoF links stay in the current snapshot",
        )

    def test_a_central_that_does_not_self_reference_fails(self):
        self.reject(
            self.mutate({0: {"FirstHaloInFOFgroup": 1}, 1: {"FirstHaloInFOFgroup": 0}}),
            "a central must self-reference",
        )

    def test_a_fof_chain_leaving_its_group_fails(self):
        self.reject(
            self.mutate({0: {"NextHaloInFOFgroup": 5}}),
            "belongs to group",
        )

    def test_a_satellite_named_twice_in_the_fof_chain_fails(self):
        self.reject(self.mutate({1: {"NextHaloInFOFgroup": 1}}), "NextHaloInFOFgroup pointer")

    def test_a_fof_cycle_fails(self):
        """Rows 5 and 6 point at each other, leaving row 4's chain short.

        Every in-degree is still exactly right, which is why the reachability
        count is not redundant with the in-degree check.
        """
        tree = self.mutate({4: {"NextHaloInFOFgroup": -1}, 6: {"NextHaloInFOFgroup": 5}})
        message = self.reject(tree, "form a cycle")
        self.assertIn("NextHaloInFOFgroup chains reach", message)

    # ---- payload --------------------------------------------------------

    def test_negative_len_fails(self):
        self.reject(self.mutate({4: {"Len": -3}}), "Len is -3")

    def test_non_finite_payload_fails_naming_the_source_row(self):
        message = self.reject(self.mutate({5: {"Vmax": float("nan")}}), "non-finite value")
        self.assertIn("row 5", message)
        self.assertIn("'Vmax'", message)

    def test_infinite_vector_component_fails(self):
        self.reject(self.mutate({2: {"Pos": (1.0, float("inf"), 3.0)}}), "non-finite value")

    def test_a_valid_tree_is_accepted_unchanged(self):
        """The control: the unmutated fixture passes every rule above."""
        self.write("trees.0", [self.mutate()])
        columns, _sizes = collect(self.adapter([(0, self.path("trees.0"))]), 4)
        self.assertEqual(columns["identity"]["SourceHaloID"].size, 7)


# ==========================================================================
# Real mini-Millennium data
# ==========================================================================

REAL_FILE = os.path.join(MINI_MILLENNIUM, "snapshots", "trees_063.0")
REAL_AVAILABLE = os.path.exists(REAL_FILE)


@unittest.skipUnless(REAL_AVAILABLE, "mini-Millennium source data is not present")
class RealMiniMillenniumTests(unittest.TestCase):
    """Complete real trees, compared against the independent extraction.

    mini-Millennium is the plan's gap-containing acceptance gate. This
    converts whole trees from file 0 and checks every emitted value and edge
    against ``struct``-level reads of the same bytes.
    """

    TREES_COMPARED = 40

    @classmethod
    def setUpClass(cls):
        cls.schema = default_schema()
        cls.adapter = LHaloBinaryAdapter(cls.schema, [(0, REAL_FILE)], max_snapshot=63)
        cls.inventory = cls.adapter.inventory()
        cls.source = read_file_directly(REAL_FILE)[: cls.TREES_COMPARED]
        cls.n_rows = sum(len(tree) for tree in cls.source)
        columns = {}
        emitted = 0
        for batch in cls.adapter.iter_batches(4096):
            for group, mapping in (
                ("identity", batch.identity),
                ("links", batch.links),
                ("payload", batch.payload),
                ("coordinates", batch.coordinates),
            ):
                for name, values in mapping.items():
                    columns.setdefault(group, {}).setdefault(name, []).append(np.asarray(values))
            emitted += batch.n_rows
            if emitted >= cls.n_rows:
                break
        cls.columns = {
            group: {name: np.concatenate(parts)[: cls.n_rows] for name, parts in mapping.items()}
            for group, mapping in columns.items()
        }

    def test_header_counts_match_the_independent_read(self):
        ntrees, total, counts = read_counts_directly(REAL_FILE)
        self.assertEqual(len(self.inventory.units), ntrees)
        self.assertEqual([unit.n_halos for unit in self.inventory.units], counts)
        self.assertEqual(self.inventory.total_halos, total)

    def test_every_value_matches_the_independent_read(self):
        payload = self.columns["payload"]
        index = 0
        for tree in self.source:
            for row in tree:
                for name in ("Len", "SnapNum", "MostBoundID"):
                    self.assertEqual(int(payload[name][index]), row[name], (index, name))
                for name in ("M_Crit200", "VelDisp", "Vmax"):
                    self.assertEqual(
                        np.float32(payload[name][index]).tobytes(),
                        struct.pack("<f", row[name]),
                        (index, name),
                    )
                for name in ("Pos", "Vel", "Spin"):
                    self.assertEqual(
                        np.asarray(payload[name][index], dtype="<f4").tobytes(),
                        struct.pack("<3f", *row[name]),
                        (index, name),
                    )
                index += 1
        self.assertEqual(index, self.n_rows)

    def test_every_edge_matches_the_independent_read(self):
        links = self.columns["links"]
        index = 0
        base = 1
        for tree in self.source:
            for row_ordinal, row in enumerate(tree):
                for name in LINK_NAMES:
                    stored = row[name]
                    expected = NULL_LINK if stored < 0 else base + stored
                    self.assertEqual(int(links[name][index]), expected, (index, name))
                    self.assertEqual(row_ordinal, index - (base - 1), index)
                index += 1
            base += len(tree)

    def test_real_forward_gaps_survive(self):
        """The gaps are real and are preserved, not adjacency-clamped."""
        links = self.columns["links"]
        snapshots = self.columns["payload"]["SnapNum"]
        ids = self.columns["identity"]["SourceHaloID"]
        position = {int(value): index for index, value in enumerate(ids)}
        spans = []
        for index in range(self.n_rows):
            target = int(links["Descendant"][index])
            if target == NULL_LINK or target not in position:
                continue
            spans.append(int(snapshots[position[target]]) - int(snapshots[index]))
        self.assertTrue(spans)
        self.assertGreaterEqual(max(spans), 2, "expected at least one forward gap")
        self.assertEqual(min(spans), 1)

    def test_identity_is_dense_and_ordered(self):
        ids = self.columns["identity"]["SourceHaloID"]
        self.assertEqual(ids.tolist(), list(range(1, self.n_rows + 1)))
        self.assertEqual(
            self.columns["coordinates"]["source_file_ordinal"].tolist(), [0] * self.n_rows
        )

    def test_file_prefix_tree_numbers_match_the_vertical_enumeration(self):
        """Cumulative tree counts over preceding files, read independently."""
        info = si.load_simulation_info(os.path.join(MINI_MILLENNIUM, "simulation_info.yaml"))
        pairs = si.lhalo_file_paths(info)
        if not all(os.path.exists(path) for _number, path in pairs):
            self.skipTest("the full mini-Millennium file set is not present")
        expected_base = 0
        bases = {}
        for number, path in pairs:
            bases[number] = expected_base
            with open(path, "rb") as handle:
                ntrees = struct.unpack("<i", handle.read(4))[0]
            expected_base += ntrees
        self.assertEqual(expected_base, 29585)

        adapter = LHaloBinaryAdapter(default_schema(), pairs, max_snapshot=63)
        seen = {}
        for batch in adapter.iter_batches(200000):
            files = batch.coordinates["source_file_ordinal"]
            units = batch.coordinates["unit_ordinal"]
            forests = batch.identity["ForestIndex"]
            first = np.asarray(units) == 0
            for file_number, forest in zip(
                np.asarray(files)[first].tolist(), np.asarray(forests)[first].tolist()
            ):
                seen.setdefault(file_number, forest)
        self.assertEqual(seen, bases)


#: The other three shipped L-Halo packages, each with the tree-name prefix
#: its `simulation_info.yaml` declares. mini-Millennium has its own class
#: above (it is the plan's gap-containing acceptance gate and gets a whole
#: file); these three are sampled, because their file 0 runs to gigabytes and
#: whole-package conversion is Slice 11's evidence, not this slice's.
OTHER_PACKAGES = (
    ("millennium", "trees_063.0", 63),
    ("micro-uchuu", "Uchuu100_Planck_lhalo_binary.0", 49),
    ("mini-uchuu", "Uchuu400_Planck_lhalo_binary.0", 49),
)


class OtherRealPackageTests(unittest.TestCase):
    """Bounded real-data checks on the three other shipped L-Halo packages.

    Each converts the first few complete trees of file 0 and compares every
    value and every edge against the same ``struct``-level oracle. This is the
    input criterion's "the binary layouts used by mini/micro-Uchuu and both
    Millennium packages" exercised on the real bytes, not just on the frozen
    layout -- micro-Uchuu contributes a gap-free anchor and mini-Uchuu
    contributes genuinely negative ``MostBoundID`` values.
    """

    TREES_COMPARED = 5

    def check_package(self, package, tree_file, max_snapshot):
        path = os.path.join(REPO_ROOT, "simulations", package, "snapshots", tree_file)
        if not os.path.exists(path):
            self.skipTest("{} source data is not present".format(package))
        properties = cs.load_source_properties(
            os.path.join(REPO_ROOT, "simulations", package, "halo_properties.yaml")
        )
        schema = cs.build_schema(
            cs.load_column_map(os.path.join(PROFILE_DIR, "lhalo_binary.yaml")), properties
        )
        source = read_file_directly(path, limit=self.TREES_COMPARED)
        n_rows = sum(len(tree) for tree in source)

        adapter = LHaloBinaryAdapter(schema, [(0, path)], max_snapshot=max_snapshot)
        collected = {}
        emitted = 0
        for batch in adapter.iter_batches(65536):
            for group, mapping in (
                ("identity", batch.identity),
                ("links", batch.links),
                ("payload", batch.payload),
            ):
                for name, values in mapping.items():
                    collected.setdefault(group, {}).setdefault(name, []).append(np.asarray(values))
            emitted += batch.n_rows
            if emitted >= n_rows:
                break
        self.assertGreaterEqual(emitted, n_rows)
        columns = {
            group: {name: np.concatenate(parts)[:n_rows] for name, parts in mapping.items()}
            for group, mapping in collected.items()
        }

        payload = columns["payload"]
        links = columns["links"]
        index = 0
        base = 1
        for tree in source:
            for row in tree:
                for name in ("Len", "SnapNum", "MostBoundID"):
                    self.assertEqual(int(payload[name][index]), row[name], (package, index, name))
                for name in ("M_Crit200", "VelDisp", "Vmax"):
                    self.assertEqual(
                        np.float32(payload[name][index]).tobytes(),
                        struct.pack("<f", row[name]),
                        (package, index, name),
                    )
                for name in ("Pos", "Vel", "Spin"):
                    self.assertEqual(
                        np.asarray(payload[name][index], dtype="<f4").tobytes(),
                        struct.pack("<3f", *row[name]),
                        (package, index, name),
                    )
                for name in LINK_NAMES:
                    stored = row[name]
                    expected = NULL_LINK if stored < 0 else base + stored
                    self.assertEqual(int(links[name][index]), expected, (package, index, name))
                index += 1
            base += len(tree)
        self.assertEqual(index, n_rows)
        self.assertEqual(columns["identity"]["SourceHaloID"].tolist(), list(range(1, n_rows + 1)))

    def test_millennium(self):
        self.check_package(*OTHER_PACKAGES[0])

    def test_micro_uchuu(self):
        self.check_package(*OTHER_PACKAGES[1])

    def test_mini_uchuu(self):
        self.check_package(*OTHER_PACKAGES[2])


if __name__ == "__main__":
    unittest.main()
