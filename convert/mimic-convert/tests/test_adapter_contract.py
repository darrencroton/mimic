"""Unit tests: the canonical record/topology contracts every adapter
must satisfy (convert/mimic-convert/adapters/base.py).

The load-bearing distinctions tested here:

- ``SourceHaloID`` is the unique positive row key; ``MostBoundID`` is source
  catalog data that may legitimately be duplicated, negative or zero. A batch
  that would be rejected if the two were confused must be accepted, and the
  test that proves it is deliberate, not an oversight.
- Prefix-sum identity is exact above 2**31 and 2**53, and overflows abort
  rather than wrap.
- Sampling narrows *what is converted*, never *what an id means*: a sampled
  run's ids equal the unsampled run's.
- Inversion is to the route's declared canonical unit: physical for the two
  prelinked routes, the whole forest ``(0, ForestIndex)`` with
  ``HaloRankInForest`` as the row for ``consistent_trees_ascii``.
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


def id_of(inventory, coordinate):
    """The ``SourceHaloID`` the inventory assigns one coordinate: its unit's
    ``base_id`` plus the within-unit row."""
    return (
        inventory.base_id(coordinate.source_file_ordinal, coordinate.unit_ordinal)
        + coordinate.row_ordinal
    )


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
            self.assertEqual(id_of(inventory, coordinate), source_halo_id)
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
        # A row one past the unit's end has no id: its would-be id lies
        # outside [1, total_halos].
        with self.assertRaisesRegex(ConverterError, "outside"):
            inventory.coordinate(inventory.base_id(0, 0) + 2)
        with self.assertRaisesRegex(ConverterError, "not in the inventory"):
            inventory.base_id(1, 0)
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

    def test_the_ascii_canonical_unit_is_the_forest_and_inverts_to_its_rank(self):
        """``consistent_trees_ascii`` declares the whole forest its canonical
        unit, keyed ``(0, ForestIndex)``: the prefix-sum arithmetic is the
        prelinked routes' unchanged, ``SourceHaloID = 1 + sum(n_g for g <
        ForestIndex) + HaloRankInForest``, and inversion lands on
        ``(0, ForestIndex, HaloRankInForest)`` -- the 0 a shim, declared by
        ``physical_units = False`` rather than read as a file."""
        counts = [6, 4, 2, 2, 3]
        inventory = base.SourceInventory(
            [base.SourceUnit(0, forest, n) for forest, n in enumerate(counts)]
        )
        inventory.physical_units = False
        source_halo_id = 1
        for forest, n in enumerate(counts):
            self.assertEqual(inventory.base_id(0, forest), 1 + sum(counts[:forest]))
            for rank in range(n):
                self.assertEqual(
                    inventory.coordinate(source_halo_id), base.SourceCoordinate(0, forest, rank)
                )
                self.assertEqual(
                    id_of(inventory, base.SourceCoordinate(0, forest, rank)), source_halo_id
                )
                source_halo_id += 1
        self.assertEqual(source_halo_id - 1, inventory.total_halos)
        # a batch carrying the shim coordinate satisfies the unchanged contract
        schema = ascii_schema()
        batch = make_batch(schema, n_rows=3)
        forest, ranks = 1, np.arange(3, dtype=np.int64)
        batch.identity["SourceHaloID"][:] = inventory.base_id(0, forest) + ranks
        batch.identity["ForestIndex"][:] = forest
        batch.identity["HaloRankInForest"][:] = ranks
        batch.links["FirstHaloInFOFgroup"][:] = batch.identity["SourceHaloID"]
        batch.coordinates["source_file_ordinal"][:] = 0
        batch.coordinates["unit_ordinal"][:] = forest
        batch.coordinates["row_ordinal"][:] = ranks
        batch.validate()


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
        """The distinction that v3 exists to make.

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

    def test_a_subclass_with_an_unknown_source_format_fails_at_instantiation(self):
        class BadAdapter(base.SourceAdapter):
            source_format = "lhalo_hdf5"

            def inventory(self):
                raise NotImplementedError

            def iter_batches(self, max_rows):
                raise NotImplementedError

        with self.assertRaisesRegex(ConverterError, "not one of"):
            BadAdapter()

    def test_an_abstract_intermediate_base_is_allowed(self):
        """A shared base that is itself abstract must not be forced to invent a
        format key it does not answer to -- only its concrete subclasses have
        one."""

        class SharedBinaryBase(base.SourceAdapter):
            """Abstract intermediate: implements one of the two abstracts."""

            def inventory(self):
                return base.SourceInventory([base.SourceUnit(0, 0, 1)])

        # Defining it must not raise, and it must still be uninstantiable
        # because iter_batches is abstract -- with the standard TypeError, not
        # a complaint about its empty format key.
        with self.assertRaises(TypeError):
            SharedBinaryBase()

        schema = ascii_schema()

        class ConcreteFromShared(SharedBinaryBase):
            source_format = "consistent_trees_ascii"

            def iter_batches(self, max_rows):
                yield make_batch(schema, n_rows=1)

        adapter = ConcreteFromShared()
        self.assertEqual(adapter.inventory().total_halos, 1)

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


class NonFinitePayloadTests(unittest.TestCase):
    """Float parsing/casts reject non-finite input and overflow and preserve
    signed zero. Rejecting NaN/infinity and *keeping* signed zero are two
    halves of one rule, so both are tested here."""

    def setUp(self):
        self.schema = ascii_schema(
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

    def test_non_finite_scalar_payload_is_rejected(self):
        for label, value in (("nan", np.nan), ("+inf", np.inf), ("-inf", -np.inf)):
            with self.subTest(value=label):
                batch = make_batch(self.schema)
                batch.payload["Vmax"][1] = value
                with self.assertRaisesRegex(ConverterError, "non-finite value"):
                    batch.validate()

    def test_non_finite_vector_payload_is_rejected_and_the_index_is_named(self):
        batch = make_batch(self.schema)
        batch.payload["Pos"][2][1] = np.nan
        with self.assertRaisesRegex(ConverterError, r"non-finite value nan at index \(2, 1\)"):
            batch.validate()

    def test_non_finite_extra_is_rejected(self):
        batch = make_batch(self.schema)
        batch.extras["Rvir"][0] = np.inf
        with self.assertRaisesRegex(ConverterError, "non-finite value"):
            batch.validate()

    def test_signed_zero_passes_and_is_preserved(self):
        """A negative zero is finite. The rule is to preserve it, not reject
        it, and validation must not normalise it away either."""
        batch = make_batch(self.schema)
        batch.payload["Vmax"][0] = -0.0
        batch.payload["Pos"][1][2] = -0.0
        batch.extras["Rvir"][0] = -0.0
        batch.validate()
        self.assertTrue(np.signbit(batch.payload["Vmax"][0]))
        self.assertTrue(np.signbit(batch.payload["Pos"][1][2]))
        self.assertTrue(np.signbit(batch.extras["Rvir"][0]))
        self.assertEqual(batch.payload["Vmax"][0], 0.0)

    def test_finite_extremes_still_pass(self):
        """The check is finiteness, not magnitude: the largest representable
        float32 is valid data."""
        batch = make_batch(self.schema)
        largest = np.finfo(np.float32).max
        batch.payload["Vmax"][0] = largest
        batch.payload["Vmax"][1] = -largest
        batch.validate()

    def test_integer_columns_are_unaffected(self):
        """Only floating dtypes are checked; an int column has no non-finite
        values to look for."""
        batch = make_batch(self.schema)
        batch.payload["Len"][0] = np.iinfo(np.int32).max
        batch.links["Descendant"][0] = 2**62
        batch.validate()


class UncoercedInputTests(unittest.TestCase):
    """An adapter handing a plain list must get this module's named error, not
    a raw AttributeError from a `.shape` dereference. Reachable only by an
    adapter bug, never by user input."""

    def setUp(self):
        self.schema = ascii_schema()

    def test_a_list_identity_column_is_coerced_not_crashed_on(self):
        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"] = [1, 2, 3]
        batch.links["FirstHaloInFOFgroup"] = np.array([1, 2, 3], dtype=np.int64)
        batch.validate()
        self.assertEqual(batch.n_rows, 3)

    def test_a_wrongly_typed_list_identity_reports_a_converter_error(self):
        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"] = [1.0, 2.0, 3.0]
        with self.assertRaisesRegex(ConverterError, "expected dtype int64"):
            batch.validate()

    def test_an_out_of_order_list_identity_is_still_checked(self):
        """Coercion must not skip the semantic checks that follow it."""
        batch = make_batch(self.schema)
        batch.identity["SourceHaloID"] = [3, 2, 1]
        batch.links["FirstHaloInFOFgroup"] = np.array([3, 2, 1], dtype=np.int64)
        with self.assertRaisesRegex(ConverterError, "strictly increasing"):
            batch.validate()


class SourceHaloIdDerivationTests(unittest.TestCase):
    """A batch's ids must be the ones its own coordinates imply.

    `CanonicalBatch` carries `coordinates` precisely so identity is
    recoverable, and the inventory's `base_id()` plus the within-unit row is
    what defines the id.
    Nothing in `CanonicalBatch.validate()` can check the two against each other
    -- it holds no inventory, and giving it one would put a whole-inventory
    lookup on every batch. The guarantee therefore lives here, at the contract
    level, where an adapter that invents its own id rule is caught.
    """

    #: Two files, so a per-file restart is visibly wrong at the second one.
    UNITS = (base.SourceUnit(0, 0, 3), base.SourceUnit(0, 1, 2), base.SourceUnit(1, 0, 4))

    def setUp(self):
        self.schema = ascii_schema()
        self.inventory = base.SourceInventory(self.UNITS)

    @staticmethod
    def _batch_for(schema, coordinates, ids):
        n = len(ids)
        batch = make_batch(schema, n_rows=n)
        batch.identity["SourceHaloID"][:] = np.array(ids, dtype=np.int64)
        batch.links["FirstHaloInFOFgroup"][:] = np.array(ids, dtype=np.int64)
        batch.coordinates["source_file_ordinal"][:] = np.array(
            [c.source_file_ordinal for c in coordinates], dtype=np.int64
        )
        batch.coordinates["unit_ordinal"][:] = np.array(
            [c.unit_ordinal for c in coordinates], dtype=np.int64
        )
        batch.coordinates["row_ordinal"][:] = np.array(
            [c.row_ordinal for c in coordinates], dtype=np.int64
        )
        return batch

    def _coordinates_of(self, unit):
        return [
            base.SourceCoordinate(unit.source_file_ordinal, unit.unit_ordinal, row)
            for row in range(unit.n_halos)
        ]

    @staticmethod
    def _mismatches(inventory, batch):
        """Rows whose id disagrees with the id their coordinate implies."""
        bad = []
        for row in range(batch.n_rows):
            coordinate = base.SourceCoordinate(
                int(batch.coordinates["source_file_ordinal"][row]),
                int(batch.coordinates["unit_ordinal"][row]),
                int(batch.coordinates["row_ordinal"][row]),
            )
            expected = id_of(inventory, coordinate)
            actual = int(batch.identity["SourceHaloID"][row])
            if expected != actual:
                bad.append((coordinate, expected, actual))
        return bad

    def test_ids_derived_through_the_inventory_validate_and_agree(self):
        for unit in self.UNITS:
            with self.subTest(unit=(unit.source_file_ordinal, unit.unit_ordinal)):
                coordinates = self._coordinates_of(unit)
                ids = [id_of(self.inventory, c) for c in coordinates]
                batch = self._batch_for(self.schema, coordinates, ids)
                batch.validate()
                self.assertEqual(self._mismatches(self.inventory, batch), [])

    def test_ids_assigned_by_a_per_file_rule_are_caught(self):
        """The counterexample: an adapter restarting ids at 1 in each file.

        Structurally the batch is fine -- ids are positive and strictly
        increasing -- so `validate()` accepts it, which is the point: the error
        is only visible against the inventory.
        """
        unit = self.UNITS[2]  # file 1, whose real ids start at 6, not 1
        coordinates = self._coordinates_of(unit)
        wrong_ids = [1 + row for row in range(unit.n_halos)]
        batch = self._batch_for(self.schema, coordinates, wrong_ids)

        batch.validate()  # structurally valid, and still wrong

        mismatches = self._mismatches(self.inventory, batch)
        self.assertEqual(len(mismatches), unit.n_halos)
        first_coordinate, expected, actual = mismatches[0]
        self.assertEqual(first_coordinate, base.SourceCoordinate(1, 0, 0))
        self.assertEqual(expected, self.inventory.base_id(1, 0))
        self.assertEqual(actual, 1)

    def test_ids_from_a_compacted_sample_inventory_are_caught(self):
        """Sampling must not compact identity.

        An adapter that rebuilt its inventory from only the units it converts
        produces ids that no longer match the parent inventory's, which is what
        makes a sampled run incomparable to an unsampled reference.
        """
        unit = self.UNITS[2]
        compacted = base.SourceInventory([base.SourceUnit(1, 0, unit.n_halos)])
        coordinates = self._coordinates_of(unit)
        ids = [id_of(compacted, c) for c in coordinates]
        batch = self._batch_for(self.schema, coordinates, ids)

        batch.validate()
        self.assertNotEqual(self._mismatches(self.inventory, batch), [])

        # Whereas the correctly sampled inventory -- parent units, narrowed
        # selection -- produces ids that do agree.
        sampled = base.SourceInventory(self.UNITS, selected=[(1, 0)])
        good = self._batch_for(self.schema, coordinates, [id_of(sampled, c) for c in coordinates])
        good.validate()
        self.assertEqual(self._mismatches(self.inventory, good), [])


if __name__ == "__main__":
    unittest.main()
