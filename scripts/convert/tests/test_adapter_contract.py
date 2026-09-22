"""Slice 2 unit tests: the canonical record/topology contracts every adapter
must satisfy (scripts/convert/adapters/base.py).

The load-bearing distinctions tested here:

- ``SourceHaloID`` is the unique positive row key; ``MostBoundID`` is source
  catalog data that may legitimately be duplicated, negative or zero. A batch
  that would be rejected if the two were confused must be accepted, and the
  test that proves it is deliberate, not an oversight.
- Prefix-sum identity is exact above 2**31 and 2**53, and overflows abort
  rather than wrap.
- Sampling narrows *what is converted*, never *what an id means*: a sampled
  run's ids equal the unsampled run's.
"""

import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
from adapters import base  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402

REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)


def ascii_schema(extras=None):
    profile = {
        "schema_version": 1,
        "source_format": "consistent_trees_ascii",
        "required_columns": {role: [role] for role in cs.REQUIRED_ROLES["consistent_trees_ascii"]},
        "extra_fields": list(extras or []),
    }
    return cs.build_schema(cs.parse_column_map(profile, origin="<test>"))


def make_batch(schema, n_rows=3, **overrides):
    """A structurally valid batch of ``n_rows`` rows, before any override.

    Row i is its own FoF central and has no other links, which is the simplest
    shape that satisfies "FirstHaloInFOFgroup is never null".
    """
    ids = np.arange(1, n_rows + 1, dtype=np.int64)
    identity = {
        "SourceHaloID": ids,
        "ForestIndex": np.zeros(n_rows, dtype=np.int64),
        "HaloRankInForest": np.arange(n_rows, dtype=np.int64),
    }
    coordinates = {
        "source_file_ordinal": np.zeros(n_rows, dtype=np.int64),
        "unit_ordinal": np.zeros(n_rows, dtype=np.int64),
        "row_ordinal": np.arange(n_rows, dtype=np.int64),
    }
    null = np.full(n_rows, base.NULL_LINK, dtype=np.int64)
    links = {
        "Descendant": null.copy(),
        "FirstProgenitor": null.copy(),
        "NextProgenitor": null.copy(),
        "FirstHaloInFOFgroup": ids.copy(),
        "NextHaloInFOFgroup": null.copy(),
    }
    payload = {}
    for field in schema.payload_fields:
        spec = cs.EXTRA_TYPES[field.type]
        shape = (n_rows,) if spec.n_components == 1 else (n_rows, spec.n_components)
        payload[field.name] = np.zeros(shape, dtype=spec.numpy_dtype)
    extras = {}
    for extra in schema.extra_fields:
        spec = extra.spec
        shape = (n_rows,) if spec.n_components == 1 else (n_rows, spec.n_components)
        extras[extra.name] = np.zeros(shape, dtype=spec.numpy_dtype)

    parts = {
        "identity": identity,
        "coordinates": coordinates,
        "links": links,
        "payload": payload,
        "extras": extras,
    }
    parts.update(overrides)
    return base.CanonicalBatch(schema=schema, **parts)


class InventoryTests(unittest.TestCase):
    def test_prefix_sums_start_at_one_and_are_contiguous(self):
        inventory = base.SourceInventory(
            [
                base.SourceUnit(0, 0, 3),
                base.SourceUnit(0, 1, 2),
                base.SourceUnit(1, 0, 4),
            ]
        )
        self.assertEqual(inventory.total_halos, 9)
        self.assertEqual(inventory.base_id(0, 0), 1)
        self.assertEqual(inventory.base_id(0, 1), 4)
        self.assertEqual(inventory.base_id(1, 0), 6)

    def test_ids_are_positive_and_round_trip_to_their_coordinate(self):
        inventory = base.SourceInventory(
            [base.SourceUnit(0, 0, 3), base.SourceUnit(0, 1, 2), base.SourceUnit(2, 5, 4)]
        )
        seen = set()
        for source_halo_id in range(1, inventory.total_halos + 1):
            coordinate = inventory.coordinate(source_halo_id)
            self.assertEqual(inventory.source_halo_id(coordinate), source_halo_id)
            self.assertNotIn(coordinate, seen)
            seen.add(coordinate)
        self.assertEqual(len(seen), inventory.total_halos)

    def test_empty_units_do_not_break_the_prefix_sums(self):
        inventory = base.SourceInventory(
            [
                base.SourceUnit(0, 0, 0),
                base.SourceUnit(0, 1, 2),
                base.SourceUnit(0, 2, 0),
                base.SourceUnit(1, 0, 1),
            ]
        )
        self.assertEqual(inventory.total_halos, 3)
        self.assertEqual(inventory.coordinate(1), base.SourceCoordinate(0, 1, 0))
        self.assertEqual(inventory.coordinate(3), base.SourceCoordinate(1, 0, 0))

    def test_out_of_range_ids_and_coordinates_fail(self):
        inventory = base.SourceInventory([base.SourceUnit(0, 0, 2)])
        for bad in (0, -1, 3):
            with self.subTest(source_halo_id=bad):
                with self.assertRaisesRegex(ConverterError, "outside"):
                    inventory.coordinate(bad)
        with self.assertRaisesRegex(ConverterError, "outside unit"):
            inventory.source_halo_id(base.SourceCoordinate(0, 0, 2))
        with self.assertRaisesRegex(ConverterError, "not in the inventory"):
            inventory.source_halo_id(base.SourceCoordinate(1, 0, 0))
        with self.assertRaisesRegex(ConverterError, "not in the inventory"):
            inventory.base_id(9, 9)

    def test_duplicate_and_out_of_order_units_fail(self):
        with self.assertRaisesRegex(ConverterError, "twice"):
            base.SourceInventory([base.SourceUnit(0, 0, 1), base.SourceUnit(0, 0, 1)])
        with self.assertRaisesRegex(ConverterError, "ascending"):
            base.SourceInventory([base.SourceUnit(0, 1, 1), base.SourceUnit(0, 0, 1)])
        with self.assertRaisesRegex(ConverterError, "ascending"):
            base.SourceInventory([base.SourceUnit(1, 0, 1), base.SourceUnit(0, 9, 1)])

    def test_negative_ordinals_and_counts_fail(self):
        with self.assertRaisesRegex(ConverterError, "non-negative"):
            base.SourceInventory([base.SourceUnit(-1, 0, 1)])
        with self.assertRaisesRegex(ConverterError, "negative halo count"):
            base.SourceInventory([base.SourceUnit(0, 0, -1)])

    def test_int64_overflow_aborts(self):
        with self.assertRaisesRegex(ConverterError, "overflows int64"):
            base.SourceInventory(
                [
                    base.SourceUnit(0, 0, base.INT64_MAX - 1),
                    base.SourceUnit(0, 1, 4),
                ]
            )

    def test_identity_is_exact_above_2_31_and_2_53(self):
        """Prefix sums are Python ints, so nothing rounds at 2**53."""
        first = 2**31 + 7
        second = 2**53 + 11
        inventory = base.SourceInventory(
            [
                base.SourceUnit(0, 0, first),
                base.SourceUnit(0, 1, second),
                base.SourceUnit(1, 0, 3),
            ]
        )
        self.assertEqual(inventory.total_halos, first + second + 3)
        self.assertEqual(inventory.base_id(0, 1), first + 1)
        self.assertEqual(inventory.base_id(1, 0), first + second + 1)

        boundary = first + 1  # first row of the second unit
        self.assertEqual(inventory.coordinate(boundary), base.SourceCoordinate(0, 1, 0))
        # 2**53 and 2**53 + 1 collapse onto one float64; they must not collapse
        # here.
        low = inventory.coordinate(2**53)
        high = inventory.coordinate(2**53 + 1)
        self.assertNotEqual(low, high)
        self.assertEqual(high.row_ordinal - low.row_ordinal, 1)

    def test_sampling_preserves_the_parent_prefix_identities(self):
        units = [
            base.SourceUnit(0, 0, 5),
            base.SourceUnit(0, 1, 7),
            base.SourceUnit(1, 0, 3),
        ]
        full = base.SourceInventory(units)
        sampled = base.SourceInventory(units, selected=[(0, 1)])
        self.assertEqual(sampled.selected, ((0, 1),))
        self.assertEqual(sampled.total_halos, full.total_halos)
        self.assertEqual(sampled.base_id(0, 1), full.base_id(0, 1))
        self.assertEqual(sampled.base_id(1, 0), full.base_id(1, 0))

    def test_selecting_an_absent_unit_fails(self):
        with self.assertRaisesRegex(ConverterError, "not in the inventory"):
            base.SourceInventory([base.SourceUnit(0, 0, 1)], selected=[(0, 4)])

    def test_default_selection_is_every_unit(self):
        inventory = base.SourceInventory([base.SourceUnit(0, 0, 1), base.SourceUnit(3, 2, 1)])
        self.assertEqual(inventory.selected, ((0, 0), (3, 2)))


class LinkFieldTableTests(unittest.TestCase):
    def test_five_links_and_three_snapshot_columns(self):
        self.assertEqual(
            base.LINK_FIELDS,
            (
                "Descendant",
                "FirstProgenitor",
                "NextProgenitor",
                "FirstHaloInFOFgroup",
                "NextHaloInFOFgroup",
            ),
        )
        self.assertEqual(
            base.SNAPSHOT_LINK_FIELDS,
            ("DescendantSnapshot", "FirstProgenitorSnapshot", "NextProgenitorSnapshot"),
        )

    def test_null_sentinel_is_minus_one(self):
        self.assertEqual(base.NULL_LINK, -1)


class CanonicalBatchTests(unittest.TestCase):
    def setUp(self):
        self.schema = ascii_schema()

    def test_valid_batch_passes(self):
        make_batch(self.schema).validate()

    def test_empty_batch_passes(self):
        batch = make_batch(self.schema, n_rows=0)
        batch.validate()
        self.assertEqual(batch.n_rows, 0)

    def test_batch_with_extras_passes_and_requires_them(self):
        schema = ascii_schema(
            [
                {
                    "name": "Rvir",
                    "sources": [{"field": "Rvir"}],
                    "type": "float",
                    "units": "kpc/h",
                    "h_convention": "carried",
                    "description": "test",
                }
            ]
        )
        make_batch(schema).validate()
        with self.assertRaisesRegex(ConverterError, "missing selected extra"):
            make_batch(schema, extras={}).validate()

    def test_unselected_extra_is_rejected(self):
        extras = {"Rvir": np.zeros(3, dtype=np.float32)}
        with self.assertRaisesRegex(ConverterError, "unselected extra"):
            make_batch(self.schema, extras=extras).validate()

    def test_most_bound_id_may_be_duplicated_negative_or_zero(self):
        """The distinction that v3 exists to make (C3).

        MostBoundID is the source catalog's signed particle identifier. It is
        not a key, so none of these values is an error -- while the same
        values in SourceHaloID are.
        """
        batch = make_batch(self.schema)
        batch.payload["MostBoundID"][:] = np.array([-5, -5, 0], dtype=np.int64)
        batch.validate()

    def test_source_halo_id_must_be_positive_and_strictly_increasing(self):
        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"][0] = 0
        with self.assertRaisesRegex(ConverterError, "must be positive"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"][:] = np.array([1, 1, 2], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "strictly increasing"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"][:] = np.array([3, 2, 1], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "strictly increasing"):
            batch.validate()

    def test_identity_and_link_columns_are_int64(self):
        batch = make_batch(self.schema)
        batch.identity["ForestIndex"] = np.zeros(3, dtype=np.int32)
        with self.assertRaisesRegex(ConverterError, "expected dtype int64"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.links["Descendant"] = np.full(3, -1, dtype=np.int32)
        with self.assertRaisesRegex(ConverterError, "expected dtype int64"):
            batch.validate()

    def test_link_values_above_2_31_and_2_53_are_accepted_exactly(self):
        batch = make_batch(self.schema)
        wide = np.array([2**31 + 1, 2**53 + 1, 2**62], dtype=np.int64)
        batch.links["Descendant"][:] = wide
        batch.validate()
        self.assertEqual(int(batch.links["Descendant"][1]), 2**53 + 1)

    def test_only_minus_one_is_null(self):
        batch = make_batch(self.schema)
        batch.links["Descendant"][0] = -2
        with self.assertRaisesRegex(ConverterError, "below the -1 null sentinel"):
            batch.validate()

    def test_link_to_source_halo_id_zero_is_rejected(self):
        batch = make_batch(self.schema)
        batch.links["Descendant"][0] = 0
        with self.assertRaisesRegex(ConverterError, "SourceHaloID 0"):
            batch.validate()

    def test_fof_central_link_is_never_null(self):
        batch = make_batch(self.schema)
        batch.links["FirstHaloInFOFgroup"][1] = base.NULL_LINK
        with self.assertRaisesRegex(ConverterError, "never null"):
            batch.validate()

    def test_missing_and_unknown_columns_fail(self):
        cases = (
            (
                "identity",
                {"SourceHaloID": np.arange(1, 4, dtype=np.int64)},
                "missing identity column",
            ),
            ("links", {"Descendant": np.full(3, -1, np.int64)}, "missing link column"),
            ("coordinates", {"row_ordinal": np.zeros(3, np.int64)}, "missing source coordinate"),
        )
        for part, replacement, pattern in cases:
            with self.subTest(part=part):
                with self.assertRaisesRegex(ConverterError, pattern):
                    make_batch(self.schema, **{part: replacement}).validate()

        batch = make_batch(self.schema)
        batch.links["Extra"] = np.zeros(3, np.int64)
        with self.assertRaisesRegex(ConverterError, "unknown link column"):
            batch.validate()

    def test_missing_source_halo_id_fails_before_anything_else(self):
        batch = make_batch(self.schema, identity={"ForestIndex": np.zeros(3, np.int64)})
        with self.assertRaisesRegex(ConverterError, "missing the SourceHaloID"):
            batch.validate()

    def test_payload_dtype_and_shape_are_checked(self):
        batch = make_batch(self.schema)
        batch.payload["M_Crit200"] = np.zeros(3, dtype=np.float64)
        with self.assertRaisesRegex(ConverterError, "expected dtype float32"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.payload["Pos"] = np.zeros(3, dtype=np.float32)
        with self.assertRaisesRegex(ConverterError, r"expected shape \(3, 3\)"):
            batch.validate()

    def test_negative_len_and_snapnum_fail(self):
        batch = make_batch(self.schema)
        batch.payload["Len"][0] = -1
        with self.assertRaisesRegex(ConverterError, "Len must never be negative"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.payload["SnapNum"][0] = -1
        with self.assertRaisesRegex(ConverterError, "SnapNum must be non-negative"):
            batch.validate()

    def test_negative_identity_and_coordinates_fail(self):
        batch = make_batch(self.schema)
        batch.identity["HaloRankInForest"][0] = -1
        with self.assertRaisesRegex(ConverterError, "must be non-negative"):
            batch.validate()

        batch = make_batch(self.schema)
        batch.coordinates["unit_ordinal"][0] = -1
        with self.assertRaisesRegex(ConverterError, "must be non-negative"):
            batch.validate()

    def test_row_counts_must_agree_across_columns(self):
        batch = make_batch(self.schema)
        batch.coordinates["row_ordinal"] = np.zeros(2, dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "expected shape"):
            batch.validate()

    def test_lhalo_payload_dtypes_come_from_the_schema(self):
        properties = cs.load_source_properties(
            os.path.join(REPO_ROOT, "simulations", "mini-millennium", "halo_properties.yaml")
        )
        profile = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": {role: [role] for role in cs.REQUIRED_ROLES["lhalo_binary"]},
            "extra_fields": [],
            "binary_layout": {
                "byte_order": "little",
                "itemsize": 104,
                "offsets": {prop.name: 0 for prop in properties},
            },
        }
        # Offsets all at 0 would overlap, so give each field its own slot.
        offset = 0
        for prop in properties:
            profile["binary_layout"]["offsets"][prop.name] = offset
            offset += cs.EXTRA_TYPES[prop.type].itemsize
        profile["binary_layout"]["itemsize"] = offset
        schema = cs.build_schema(cs.parse_column_map(profile, origin="<test>"), properties)
        batch = make_batch(schema)
        batch.validate()
        self.assertEqual(batch.payload["M_Crit200"].dtype, np.dtype("float32"))
        self.assertEqual(batch.payload["MostBoundID"].dtype, np.dtype("int64"))


class SourceAdapterTests(unittest.TestCase):
    def test_the_base_class_cannot_be_instantiated(self):
        with self.assertRaises(TypeError):
            base.SourceAdapter()

    def test_a_subclass_with_an_unknown_source_format_fails_at_definition(self):
        with self.assertRaisesRegex(ConverterError, "not one of"):

            class BadAdapter(base.SourceAdapter):
                source_format = "lhalo_hdf5"

                def inventory(self):
                    raise NotImplementedError

                def iter_batches(self, max_rows):
                    raise NotImplementedError

    def test_a_conforming_subclass_defines_and_instantiates(self):
        schema = ascii_schema()

        class StubAdapter(base.SourceAdapter):
            source_format = "consistent_trees_ascii"

            def inventory(self):
                return base.SourceInventory([base.SourceUnit(0, 0, 3)])

            def iter_batches(self, max_rows):
                yield make_batch(schema)

        adapter = StubAdapter()
        self.assertEqual(adapter.inventory().total_halos, 3)
        batches = list(adapter.iter_batches(max_rows=10))
        self.assertEqual(len(batches), 1)
        batches[0].validate()

    def test_an_incomplete_subclass_cannot_be_instantiated(self):
        class PartialAdapter(base.SourceAdapter):
            source_format = "consistent_trees_ascii"

            def inventory(self):
                return base.SourceInventory([])

        with self.assertRaises(TypeError):
            PartialAdapter()


if __name__ == "__main__":
    unittest.main()
