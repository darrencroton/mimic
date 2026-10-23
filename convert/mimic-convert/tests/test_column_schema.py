"""Unit tests for the canonical source schema, mapping profiles and schema
identity (convert/mimic-convert/column_schema.py).

Three properties carry most of the weight here and are tested directly rather
than inferred:

- **Presentation does not move the digest.** Comments, key order, alias order
  and extra-definition order are normalised away; anything that changes how a
  value is read, typed, named or labelled is not.
- **Same-width substitutions are visible.** ``int``/``float``,
  ``long long``/``double`` and ``vec3_int``/``vec3_float`` occupy the same
  bytes and must still produce different digests, because the conversion
  manifest's resume path must reject a same-width schema substitution before
  it mutates anything.
- **Integers never pass through floating point.** Offsets, itemsizes and
  declared values above 2**31 and 2**53 survive canonical serialization
  exactly, and 2**53 and 2**53 + 1 are distinguishable.

The shipped profiles under convert/mimic-convert/profiles/ are loaded on every run,
so documentation that stops being runnable fails here rather than in a later
conversion.
"""

import dataclasses
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
import ctrees_parser  # noqa: E402
from adapters import source_inventory as si  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
PROFILE_DIR = REPO_ROOT / "convert" / "mimic-convert" / "profiles"
DATA_DIR = Path(__file__).parent / "data" / "column_maps"
LHALO_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"
CTREES_HDF5_READER = REPO_ROOT / "src" / "io" / "vertical" / "read_ctrees_hdf5.c"


def valid_profile(source_format, extras=None):
    """A minimal valid profile dict: every role mapped to its own name."""
    profile = {
        "schema_version": 1,
        "source_format": source_format,
        "required_columns": {role: [role] for role in cs.REQUIRED_ROLES[source_format]},
        "extra_fields": list(extras or []),
    }
    if source_format == "lhalo_binary":
        profile["binary_layout"] = {
            "byte_order": "little",
            "itemsize": 104,
            "offsets": {
                "Descendant": 0,
                "FirstProgenitor": 4,
                "NextProgenitor": 8,
                "FirstHaloInFOFgroup": 12,
                "NextHaloInFOFgroup": 16,
                "Len": 20,
                "M_Mean200": 24,
                "M_Crit200": 28,
                "M_TopHat": 32,
                "Pos": 36,
                "Vel": 48,
                "VelDisp": 60,
                "Vmax": 64,
                "Spin": 68,
                "MostBoundID": 80,
                "SnapNum": 88,
                "FileNr": 92,
                "SubhaloIndex": 96,
                "SubHalfMass": 100,
            },
        }
    return profile


def scalar_extra(
    name="Extra", field="Rvir", type_name="float", units="kpc/h", convention="carried"
):
    return {
        "name": name,
        "sources": [{"field": field}],
        "type": type_name,
        "units": units,
        "h_convention": convention,
        "description": "test extra",
    }


def schema_for(profile, source_properties=None):
    column_map = cs.parse_column_map(profile, origin="<test>")
    return cs.build_schema(column_map, source_properties)


class ShippedProfileTests(unittest.TestCase):
    """Every profile the repository ships must load through the pure loader."""

    @classmethod
    def setUpClass(cls):
        cls.source_properties = cs.load_source_properties(LHALO_PROPERTIES)

    def test_every_shipped_profile_loads_and_builds(self):
        paths = sorted(PROFILE_DIR.glob("*.yaml"))
        self.assertTrue(paths, "no profiles found under convert/mimic-convert/profiles/")
        for path in paths:
            with self.subTest(profile=path.name):
                column_map = cs.load_column_map(path)
                properties = (
                    self.source_properties if column_map.source_format == "lhalo_binary" else None
                )
                schema = cs.build_schema(column_map, properties)
                self.assertEqual(len(schema.digest), 64)
                self.assertEqual(schema.digest, schema.digest.lower())

    def test_no_shipped_extra_collides_with_a_core_property_name(self):
        """The reserved-name check covers what the *format* reserves, not the
        consuming package's vocabulary -- so a shipped example that collided
        with a Mimic core property would fail at package-generation time, not
        at conversion time. `Rvir` did; it is now `CatalogRvir`.
        """
        core = yaml.safe_load((REPO_ROOT / "src" / "core" / "core_properties.yaml").read_text())
        core_names = {entry["name"] for entry in core["halo_properties"]}
        self.assertIn("Rvir", core_names, "guard assumes core_properties.yaml declares Rvir")
        for path in sorted(PROFILE_DIR.glob("*.yaml")):
            column_map = cs.load_column_map(path)
            for extra in column_map.extra_fields:
                with self.subTest(profile=path.name, extra=extra.name):
                    self.assertNotIn(extra.name, core_names)

    def test_shipped_hdf5_example_types_match_the_real_source_dtypes(self):
        """The real micro-Uchuu source stores Forests/Rvir and Forests/Spin as
        float64 and pid as int64, so the example must declare double and
        long long. Declaring float32 would narrow every value silently.

        Asserted against the shipped profile rather than by opening the 13 GB
        dataset: the dtypes were verified directly against
        simulations/micro-uchuu-hdf5/snapshots/MicroUchuu_mergertree_info.h5
        when this expectation was written, and pinning them here keeps the
        example honest without making the unit suite depend on real data.
        """
        column_map = cs.load_column_map(PROFILE_DIR / "consistent_trees_hdf5_extras_example.yaml")
        declared = {extra.name: extra.type for extra in column_map.extra_fields}
        self.assertEqual(declared["CatalogRvir"], "double")
        self.assertEqual(declared["SpinParameter"], "double")
        self.assertEqual(declared["ParentID"], "long long")

    def test_shipped_binary_layout_matches_the_shipped_104_byte_record(self):
        schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary.yaml"), self.source_properties
        )
        layout = schema.source_layout
        self.assertEqual(layout.itemsize, 104)
        self.assertEqual(layout.byte_order, "little")
        self.assertEqual(layout.numpy_byte_order, "<")
        self.assertEqual(len(layout.entries), len(self.source_properties))
        covered = sum(entry.itemsize for entry in layout.entries)
        self.assertEqual(covered, 104, "the shipped record is fully packed, with no padding")

    def test_extra_selection_never_changes_the_on_disk_stride(self):
        """Unselected source fields keep their bytes in the layout, and selecting
        an extra marks its field selected without moving any offset."""
        default = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary.yaml"), self.source_properties
        )
        with_extras = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary_extras_example.yaml"),
            self.source_properties,
        )
        entries = {entry.name: entry for entry in default.source_layout.entries}
        for unselected in ("M_Mean200", "M_TopHat", "FileNr", "SubhaloIndex", "SubHalfMass"):
            self.assertIn(unselected, entries)
            self.assertFalse(entries[unselected].selected)
        self.assertEqual(default.source_layout.itemsize, 104)
        self.assertEqual(default.source_layout.itemsize, with_extras.source_layout.itemsize)
        self.assertEqual(
            [e.offset for e in default.source_layout.entries],
            [e.offset for e in with_extras.source_layout.entries],
        )
        selected = {e.name for e in with_extras.source_layout.entries if e.selected}
        self.assertIn("M_Mean200", selected)
        self.assertNotEqual(default.digest, with_extras.digest)


class ProfileGrammarTests(unittest.TestCase):
    def test_minimal_profiles_are_valid_for_every_source_format(self):
        properties = cs.load_source_properties(LHALO_PROPERTIES)
        for source_format in cs.SOURCE_FORMATS:
            with self.subTest(source_format=source_format):
                extras = properties if source_format == "lhalo_binary" else None
                schema = schema_for(valid_profile(source_format), extras)
                self.assertEqual(schema.source_format, source_format)

    def test_a_malformed_profile_is_rejected_with_a_named_reason(self):
        """The strict grammar refuses typo and missing keys, another schema version or source
        format, a null extra list, and bad role tables or alias lists."""
        cases = (
            ("unknown top-level key", lambda p: p.update(column_map_version=1), "unknown key"),
            (
                "missing schema_version",
                lambda p: p.pop("schema_version"),
                "missing required key",
            ),
            (
                "missing required_columns",
                lambda p: p.pop("required_columns"),
                "missing required key",
            ),
            ("missing extra_fields", lambda p: p.pop("extra_fields"), "missing required key"),
            ("null extra_fields", lambda p: p.update(extra_fields=None), "must be a list"),
            ("wrong schema_version", lambda p: p.update(schema_version=2), "schema_version"),
            (
                "unknown source_format",
                lambda p: p.update(source_format="lhalo_hdf5"),
                "source_format",
            ),
            (
                "missing required role",
                lambda p: p["required_columns"].pop("snap"),
                "missing required key",
            ),
            (
                "unknown required role",
                lambda p: p["required_columns"].update(rvir=["Rvir"]),
                "unknown key",
            ),
            (
                "empty alias list",
                lambda p: p["required_columns"].update(snap=[]),
                "empty alias list",
            ),
            (
                "bare string alias list",
                lambda p: p["required_columns"].update(snap="snap_num"),
                "list of aliases",
            ),
        )
        for label, mutate, pattern in cases:
            with self.subTest(case=label):
                profile = valid_profile("consistent_trees_ascii")
                mutate(profile)
                with self.assertRaisesRegex(ConverterError, pattern):
                    schema_for(profile)

    def test_aliases_that_normalize_to_the_same_name_fail(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["mvir"] = ["Mvir", "mvir(10)"]
        with self.assertRaisesRegex(ConverterError, "twice after normalization"):
            schema_for(profile)


class ExtraFieldTests(unittest.TestCase):
    def test_every_supported_type_round_trips(self):
        for type_name, spec in cs.EXTRA_TYPES.items():
            with self.subTest(type=type_name):
                sources = [{"field": "Rvir"}]
                if spec.n_components == 3:
                    sources = [{"field": "x"}, {"field": "y"}, {"field": "z"}]
                profile = valid_profile(
                    "consistent_trees_ascii",
                    [
                        {
                            "name": "Selected",
                            "sources": sources,
                            "type": type_name,
                            "units": "dimensionless",
                            "h_convention": "none",
                            "description": "test",
                        }
                    ],
                )
                schema = schema_for(profile)
                extra = schema.extra_fields[0]
                self.assertEqual(extra.spec.numpy_dtype, spec.numpy_dtype)
                self.assertEqual(len(extra.sources), spec.n_components)

    def test_a_malformed_extra_is_rejected_with_a_named_reason(self):
        """An unsupported or unhashable type, an unknown h_convention, blank units or
        description, a bad component index or source key, and any missing or unknown entry
        key are refused."""
        component = dict(scalar_extra(units="Mpc/h"), sources=[{"field": "Pos", "component": 3}])
        cases = [
            ("unsupported type", scalar_extra(type_name="float16"), "unsupported type"),
            ("list type", dict(scalar_extra(), type=["float"]), "unsupported type"),
            ("mapping type", dict(scalar_extra(), type={"float": 1}), "unsupported type"),
            ("h_convention", scalar_extra(convention="comoving"), "h_convention"),
            ("blank units", dict(scalar_extra(), units="  "), "nonempty string"),
            ("blank description", dict(scalar_extra(), description="  "), "nonempty string"),
            ("component above 2", component, "component must be 0, 1 or 2"),
            (
                "float component",
                dict(component, sources=[{"field": "Pos", "component": 1.0}]),
                "must be an integer",
            ),
            (
                "unknown source key",
                dict(scalar_extra(), sources=[{"field": "Rvir", "index": 0}]),
                "unknown source key",
            ),
            ("unknown entry key", dict(scalar_extra(), output_convert="scale"), "unknown key"),
        ]
        for key in cs._EXTRA_ENTRY_KEYS:
            entry = {name: value for name, value in scalar_extra().items() if name != key}
            cases.append(("missing " + key, entry, "missing required key"))
        for label, extra, pattern in cases:
            with self.subTest(case=label):
                profile = valid_profile("consistent_trees_ascii", [extra])
                with self.assertRaisesRegex(ConverterError, pattern):
                    schema_for(profile)

    def test_wrong_component_count_fails(self):
        for type_name, sources in (
            ("float", [{"field": "x"}, {"field": "y"}]),
            ("vec3_float", [{"field": "x"}]),
            ("vec3_int", [{"field": "x"}, {"field": "y"}, {"field": "z"}, {"field": "x"}]),
        ):
            with self.subTest(type=type_name):
                profile = valid_profile(
                    "consistent_trees_ascii",
                    [
                        {
                            "name": "Selected",
                            "sources": sources,
                            "type": type_name,
                            "units": "dimensionless",
                            "h_convention": "none",
                            "description": "test",
                        }
                    ],
                )
                with self.assertRaisesRegex(ConverterError, "source component"):
                    schema_for(profile)

    def test_repeated_source_component_in_one_field_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["extra_fields"] = [
            {
                "name": "Selected",
                "sources": [{"field": "x"}, {"field": "x"}, {"field": "z"}],
                "type": "vec3_float",
                "units": "Mpc/h",
                "h_convention": "carried",
                "description": "test",
            }
        ]
        with self.assertRaisesRegex(ConverterError, "more than once"):
            schema_for(profile)

    def test_duplicate_extra_names_fail(self):
        profile = valid_profile(
            "consistent_trees_ascii", [scalar_extra(name="Rvir"), scalar_extra(name="Rvir")]
        )
        with self.assertRaisesRegex(ConverterError, "duplicates the output name"):
            schema_for(profile)

    def test_reserved_names_are_rejected(self):
        reserved = sorted(cs.RESERVED_OUTPUT_NAMES)
        self.assertIn("Descendant", reserved)
        self.assertIn("SourceHaloID", reserved)
        self.assertIn("MostBoundID", reserved)
        self.assertIn("DescendantSnapshot", reserved)
        self.assertIn("SourceFileOrdinal", reserved)
        for name in reserved:
            with self.subTest(name=name):
                profile = valid_profile("consistent_trees_ascii", [scalar_extra(name=name)])
                with self.assertRaisesRegex(ConverterError, "reserved"):
                    schema_for(profile)

    def test_invalid_output_names_are_rejected(self):
        # "Rvir\n" is the one that matters: `$` also matches immediately
        # before a trailing newline, so an unanchored `re.match` accepts it.
        for name in (
            "0Extra",
            "extra-field",
            "extra field",
            "_Extra",
            "Exträ",
            "Rvir\n",
            "Rvir\n\n",
            "Rvir\r\n",
        ):
            with self.subTest(name=name):
                profile = valid_profile("consistent_trees_ascii", [scalar_extra(name=name)])
                with self.assertRaisesRegex(ConverterError, "not ASCII"):
                    schema_for(profile)

    def test_output_name_above_63_bytes_is_rejected(self):
        ok_name = "E" + "x" * 62
        self.assertEqual(len(ok_name), 63)
        schema_for(valid_profile("consistent_trees_ascii", [scalar_extra(name=ok_name)]))
        too_long = "E" + "x" * 63
        profile = valid_profile("consistent_trees_ascii", [scalar_extra(name=too_long)])
        with self.assertRaisesRegex(ConverterError, "63-byte limit"):
            schema_for(profile)


class BinaryLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_properties = cs.load_source_properties(LHALO_PROPERTIES)

    def test_layout_and_source_properties_apply_to_binary_profiles_only(self):
        """A binary profile needs both binary_layout and halo_properties.yaml; no other format
        accepts either."""
        with_layout = valid_profile("consistent_trees_ascii")
        with_layout["binary_layout"] = {
            "byte_order": "little",
            "itemsize": 104,
            "offsets": {"x": 0},
        }
        without_layout = valid_profile("lhalo_binary")
        del without_layout["binary_layout"]
        cases = (
            ("layout on a non-binary profile", with_layout, None, "unknown key"),
            (
                "binary profile without layout",
                without_layout,
                self.source_properties,
                "missing required key",
            ),
            (
                "binary profile without source properties",
                valid_profile("lhalo_binary"),
                None,
                "halo_properties.yaml",
            ),
            (
                "source properties on a non-binary profile",
                valid_profile("consistent_trees_ascii"),
                self.source_properties,
                "lhalo_binary profiles only",
            ),
        )
        for label, profile, properties, pattern in cases:
            with self.subTest(case=label):
                with self.assertRaisesRegex(ConverterError, pattern):
                    schema_for(profile, properties)

    def test_invalid_byte_order_fails(self):
        for byte_order in ("native", "=", "<", None, ["little"]):
            with self.subTest(byte_order=byte_order):
                profile = valid_profile("lhalo_binary")
                profile["binary_layout"]["byte_order"] = byte_order
                with self.assertRaisesRegex(ConverterError, "byte_order"):
                    schema_for(profile, self.source_properties)

    def test_big_endian_is_supported_changes_the_identity_and_sets_the_dtype_order(self):
        little = schema_for(valid_profile("lhalo_binary"), self.source_properties)
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["byte_order"] = "big"
        big = schema_for(profile, self.source_properties)
        self.assertEqual(big.source_layout.numpy_byte_order, ">")
        self.assertNotEqual(little.digest, big.digest)
        dtype = np.dtype(big.source_layout.numpy_dtype_spec())
        self.assertEqual(dtype["Descendant"].str, ">i4")
        self.assertEqual(dtype["MostBoundID"].str, ">i8")

    def test_malformed_itemsize_and_offsets_fail(self):
        """Non-integer, non-positive, out-of-range (2**70) and mixed-type itemsize and offsets
        are refused as converter errors."""
        cases = (
            ("float itemsize", lambda l: l.update(itemsize=104.0), "must be an integer"),
            ("bool itemsize", lambda l: l.update(itemsize=True), "must be an integer"),
            ("zero itemsize", lambda l: l.update(itemsize=0), "must be in"),
            ("absurd itemsize", lambda l: l.update(itemsize=2**70), "must be in"),
            (
                "float offset",
                lambda l: l["offsets"].update(Descendant=0.0),
                "must be an integer",
            ),
            ("negative offset", lambda l: l["offsets"].update(Descendant=-4), "must be in"),
            ("absurd offset", lambda l: l["offsets"].update(Descendant=2**70), "must be in"),
            (
                "non-string offset key",
                lambda l: l["offsets"].update({7: 0}),
                "non-string key",
            ),
        )
        for label, mutate, pattern in cases:
            with self.subTest(case=label):
                profile = valid_profile("lhalo_binary")
                mutate(profile["binary_layout"])
                with self.assertRaisesRegex(ConverterError, pattern):
                    schema_for(profile, self.source_properties)

    def test_padding_is_allowed_and_the_record_is_read_with_its_declared_stride(self):
        """A record may carry bytes this converter never reads, between fields or after the
        last one, and must not be read with a packed stride."""
        for itemsize, offsets in ((112, {"SubHalfMass": 108}), (128, {})):
            with self.subTest(itemsize=itemsize):
                profile = valid_profile("lhalo_binary")
                profile["binary_layout"]["itemsize"] = itemsize
                profile["binary_layout"]["offsets"].update(offsets)
                schema = schema_for(profile, self.source_properties)
                self.assertEqual(schema.source_layout.itemsize, itemsize)
                dtype = np.dtype(schema.source_layout.numpy_dtype_spec())
                self.assertEqual(dtype.itemsize, itemsize)

    def test_layout_fixtures_fail_with_a_named_reason(self):
        cases = {
            "lhalo_overlapping_offsets.yaml": "overlap",
            "lhalo_offset_out_of_record.yaml": "record is only",
            "lhalo_missing_offset.yaml": "does not cover source field",
            "lhalo_unknown_offset_field.yaml": "source properties do not",
        }
        for name, pattern in cases.items():
            with self.subTest(fixture=name):
                column_map = cs.load_column_map(DATA_DIR / name)
                with self.assertRaisesRegex(ConverterError, pattern):
                    cs.build_schema(column_map, self.source_properties)

    def test_numpy_dtype_spec_is_consumable_and_carries_the_declared_byte_order(self):
        schema = schema_for(valid_profile("lhalo_binary"), self.source_properties)
        dtype = np.dtype(schema.source_layout.numpy_dtype_spec())
        self.assertEqual(dtype.itemsize, 104)
        self.assertEqual(dtype["Descendant"].str, "<i4")
        self.assertEqual(dtype["MostBoundID"].str, "<i8")
        self.assertEqual(dtype["Pos"].subdtype[0].str, "<f4")
        self.assertEqual(dtype["Pos"].shape, (3,))
        self.assertEqual(dtype.fields["MostBoundID"][1], 80)
        self.assertTrue(all(dtype[name].str.startswith("<") for name in ("Len", "Vmax", "SnapNum")))

    def test_unresolvable_required_alias_fails_at_freeze_time(self):
        """A binary profile's aliases must name a property's `name:`, not its
        `source:`. `M_Crit200: [Mvir]` is the plausible typo, since
        mini-millennium declares M_Crit200 with `source: Mvir`."""
        profile = valid_profile("lhalo_binary")
        profile["required_columns"]["M_Crit200"] = ["Mvir"]
        with self.assertRaisesRegex(ConverterError, "none of the aliases"):
            schema_for(profile, self.source_properties)

    def test_unresolvable_extra_source_fails_at_freeze_time(self):
        profile = valid_profile("lhalo_binary", [scalar_extra(field="NotAField")])
        with self.assertRaisesRegex(ConverterError, "does not carry field"):
            schema_for(profile, self.source_properties)

    def test_a_component_index_must_match_the_source_field_arity(self):
        """A component on a scalar source field is refused, and so is a whole vector source
        with no component."""
        cases = (
            ("M_Mean200", 1, "1e10 Msun/h", "must not carry a 'component'"),
            ("Pos", None, "Mpc/h", "needs an explicit 'component"),
        )
        for field, component, units, pattern in cases:
            with self.subTest(field=field):
                source = {"field": field}
                if component is not None:
                    source["component"] = component
                profile = valid_profile("lhalo_binary")
                profile["extra_fields"] = [dict(scalar_extra(units=units), sources=[source])]
                with self.assertRaisesRegex(ConverterError, pattern):
                    schema_for(profile, self.source_properties)

    def test_required_role_from_a_wrong_typed_source_field_fails(self):
        """An int role filled from a float32 source field (integers never
        pass through floating point)."""
        profile = valid_profile("lhalo_binary")
        profile["required_columns"]["Len"] = ["M_Mean200"]
        with self.assertRaisesRegex(ConverterError, "which is float, but the role is int"):
            schema_for(profile, self.source_properties)

    def test_required_role_from_a_vector_source_field_fails(self):
        """A scalar role filled from a stored 3-vector.

        Their *element* types are both float32, so only comparing the whole
        type -- arity included -- catches this.
        """
        profile = valid_profile("lhalo_binary")
        profile["required_columns"]["M_Crit200"] = ["Pos"]
        with self.assertRaisesRegex(ConverterError, "which is vec3_float, but the role is float"):
            schema_for(profile, self.source_properties)

    def test_extra_declaring_a_type_the_source_does_not_store_fails(self):
        profile = valid_profile(
            "lhalo_binary",
            [scalar_extra(field="FileNr", type_name="float", units="dimensionless")],
        )
        with self.assertRaisesRegex(
            ConverterError, "stores int .int32., but the extra is declared float"
        ):
            schema_for(profile, self.source_properties)

    def test_extra_narrowing_an_integer_source_fails(self):
        """`long long` selected as `int` would narrow silently."""
        profile = valid_profile(
            "lhalo_binary",
            [scalar_extra(field="MostBoundID", type_name="int", units="dimensionless")],
        )
        with self.assertRaisesRegex(ConverterError, "stores long long"):
            schema_for(profile, self.source_properties)

    def test_role_source_types_agree_with_the_payload_declarations(self):
        """The role type table and PAYLOAD_FIELDS must not drift apart."""
        payload = {field.name: field.type for field in cs.PAYLOAD_FIELDS["lhalo_binary"]}
        role_types = cs.ROLE_SOURCE_TYPES["lhalo_binary"]
        self.assertEqual(set(role_types), set(cs.REQUIRED_ROLES["lhalo_binary"]))
        for name, type_name in payload.items():
            # L-Halo applies no conversion, so a payload field's emitted type
            # is its source type.
            self.assertEqual(role_types[name], type_name, "role type differs for " + name)
        for field in cs.TOPOLOGY_FIELDS:
            if field.type != "long long":
                continue
            # int32 on disk; widened to int64 only in v3's output.
            self.assertEqual(role_types[field.name], "int", "link type differs for " + field.name)

    def test_offsets_out_of_declaration_order_fail(self):
        """Coverage, extent and overlap all pass when two neighbours trade places -- every byte
        is still claimed exactly once. Only the declaration-order check catches it, whether the
        neighbours have the same width or different widths (Spin, 12 B, and MostBoundID, 8 B,
        exchanged inside [68, 88))."""
        swaps = (
            ("same width", {"M_Mean200": 32, "M_TopHat": 24}),
            ("different width", {"MostBoundID": 68, "Spin": 76}),
        )
        for label, moved in swaps:
            with self.subTest(case=label):
                profile = valid_profile("lhalo_binary")
                profile["binary_layout"]["offsets"].update(moved)
                with self.assertRaisesRegex(ConverterError, "declaration order"):
                    schema_for(profile, self.source_properties)

    def test_two_roles_claiming_one_source_field_fails(self):
        """`Len: [SnapNum]` passes every per-role check -- both are int -- and
        would emit snapshot numbers as particle counts."""
        profile = valid_profile("lhalo_binary")
        profile["required_columns"]["Len"] = ["SnapNum"]
        with self.assertRaisesRegex(ConverterError, "both resolve to the same source field"):
            schema_for(profile, self.source_properties)

    def test_a_source_field_may_feed_a_role_and_extras(self):
        """Role-to-extra and extra-to-extra reuse is legal; only role-to-role collisions are
        rejected."""
        profile = valid_profile(
            "lhalo_binary",
            [
                scalar_extra(name="NativeMass", field="M_Crit200", units="1e10 Msun/h"),
                scalar_extra(name="MassA", field="M_Mean200", units="1e10 Msun/h"),
                scalar_extra(name="MassB", field="M_Mean200", units="1e10 Msun/h"),
            ],
        )
        schema = schema_for(profile, self.source_properties)
        self.assertEqual(
            {extra.name: extra.sources[0].field for extra in schema.extra_fields},
            {"NativeMass": "M_Crit200", "MassA": "M_Mean200", "MassB": "M_Mean200"},
        )

    def test_layout_dtype_matches_the_hard_coded_lhalo_record_dtype(self):
        """Two descriptions of the same 104-byte record must not drift apart.

        `adapters/source_inventory.py` hard-codes LHALO_FIELDS; the layout module
        derives the layout from halo_properties.yaml plus a profile. They agree
        today, and this assertion is what keeps a future property addition from
        being picked up by one and not the other.
        """
        schema = schema_for(valid_profile("lhalo_binary"), self.source_properties)
        derived = np.dtype(schema.source_layout.numpy_dtype_spec())
        hard_coded = si.lhalo_record_dtype("<")
        self.assertEqual(derived.itemsize, hard_coded.itemsize)
        self.assertEqual(list(derived.names), list(hard_coded.names))
        for name in hard_coded.names:
            self.assertEqual(
                derived.fields[name][0], hard_coded.fields[name][0], "dtype differs for " + name
            )
            self.assertEqual(
                derived.fields[name][1], hard_coded.fields[name][1], "offset differs for " + name
            )


class YamlLoadingTests(unittest.TestCase):
    def test_duplicate_yaml_keys_fail(self):
        for name in ("duplicate_top_level_key.yaml", "duplicate_extra_key.yaml"):
            with self.subTest(fixture=name):
                with self.assertRaisesRegex(ConverterError, "duplicate key"):
                    cs.load_column_map(DATA_DIR / name)

    def test_an_unusable_profile_file_fails_as_a_converter_error(self):
        """Malformed YAML, a non-mapping document and a missing file are converter errors."""
        with tempfile.TemporaryDirectory() as workdir:
            cases = (
                ("malformed YAML", DATA_DIR / "malformed.yaml", "invalid YAML"),
                ("non-mapping document", DATA_DIR / "not_a_mapping.yaml", "must be a YAML mapping"),
                ("missing file", Path(workdir) / "absent.yaml", "cannot read"),
            )
            for label, path, pattern in cases:
                with self.subTest(case=label):
                    with self.assertRaisesRegex(ConverterError, pattern):
                        cs.load_column_map(path)

    def test_invalid_utf8_profile_fails_as_a_converter_error(self):
        """Written as raw bytes in a temp file rather than committed: a
        deliberately undecodable fixture is awkward to carry in the repo and
        confuses editors and diff tools."""
        with tempfile.TemporaryDirectory() as workdir:
            path = Path(workdir) / "bad_encoding.yaml"
            path.write_bytes(b"schema_version: 1\nsource_format: \xff\xfe not utf-8\n")
            with self.assertRaisesRegex(ConverterError, "not valid UTF-8"):
                cs.load_column_map(path)

    def test_a_valid_utf8_profile_with_non_ascii_prose_still_loads(self):
        """The encoding is pinned to UTF-8, not narrowed to ASCII: a
        description may legitimately carry non-ASCII text."""
        with tempfile.TemporaryDirectory() as workdir:
            path = Path(workdir) / "unicode_ok.yaml"
            profile = valid_profile(
                "consistent_trees_ascii",
                [dict(scalar_extra(), description="virial radius, \u00c5ngstr\u00f6m-free")],
            )
            path.write_text(yaml.safe_dump(profile, allow_unicode=True), encoding="utf-8")
            schema = cs.build_schema(cs.load_column_map(path))
            self.assertIn("\u00c5", schema.extra_fields[0].description)


class SourcePropertyLoadingTests(unittest.TestCase):
    """Rejection branches of `load_source_properties`.

    Written tempdir-based rather than as committed fixtures: these are
    one-line malformed shapes with no reuse value, and a directory of
    deliberately broken `halo_properties.yaml` files invites someone to mistake
    one for a real package declaration.
    """

    #: (label, file body, expected message fragment)
    REJECTIONS = (
        ("absent key", "other_key: []\n", "must be a nonempty list"),
        ("null value", "halo_properties:\n", "must be a nonempty list"),
        ("empty list", "halo_properties: []\n", "must be a nonempty list"),
        ("mapping not list", "halo_properties:\n  name: Descendant\n", "must be a nonempty list"),
        (
            "entry is not a mapping",
            "halo_properties:\n  - Descendant\n",
            "must be a mapping",
        ),
        (
            "duplicate name",
            "halo_properties:\n"
            "  - {name: Descendant, type: int, units: dimensionless}\n"
            "  - {name: Descendant, type: int, units: dimensionless}\n",
            "duplicate property name",
        ),
        (
            "missing name",
            "halo_properties:\n  - {type: int, units: dimensionless}\n",
            "must be a nonempty string",
        ),
        (
            "unsupported type",
            "halo_properties:\n  - {name: Descendant, type: int16, units: dimensionless}\n",
            "unsupported type",
        ),
        (
            "non-string type",
            "halo_properties:\n  - {name: Descendant, type: [int], units: dimensionless}\n",
            "unsupported type",
        ),
        (
            "empty units",
            'halo_properties:\n  - {name: Descendant, type: int, units: "  "}\n',
            "must be a nonempty string",
        ),
        (
            "missing units",
            "halo_properties:\n  - {name: Descendant, type: int}\n",
            "must be a nonempty string",
        ),
        (
            "document is not a mapping",
            "- Descendant\n- FirstProgenitor\n",
            "must be a YAML mapping",
        ),
    )

    def _write(self, workdir, body):
        path = Path(workdir) / "halo_properties.yaml"
        path.write_text(body, encoding="utf-8")
        return path

    def test_unusable_declarations_are_rejected(self):
        for label, body, pattern in self.REJECTIONS:
            with self.subTest(case=label):
                with tempfile.TemporaryDirectory() as workdir:
                    path = self._write(workdir, body)
                    with self.assertRaisesRegex(ConverterError, pattern):
                        cs.load_source_properties(path)
        with self.subTest(case="missing file"):
            with tempfile.TemporaryDirectory() as workdir:
                with self.assertRaisesRegex(ConverterError, "cannot read"):
                    cs.load_source_properties(Path(workdir) / "absent.yaml")

    def test_a_valid_declaration_loads_in_declaration_order(self):
        """The positive control. Order is load-bearing -- it is the record's
        field order for a fixed-record binary source -- so it is asserted, not
        assumed."""
        with tempfile.TemporaryDirectory() as workdir:
            path = self._write(
                workdir,
                "halo_properties:\n"
                "  - {name: Descendant, type: int, units: dimensionless}\n"
                "  - {name: Pos, type: vec3_float, units: Mpc/h}\n"
                "  - {name: MostBoundID, type: long long, units: dimensionless}\n",
            )
            properties = cs.load_source_properties(path)
        self.assertEqual([p.name for p in properties], ["Descendant", "Pos", "MostBoundID"])
        self.assertEqual([p.type for p in properties], ["int", "vec3_float", "long long"])
        self.assertEqual(properties[1].units, "Mpc/h")


class SchemaIdentityTests(unittest.TestCase):
    def test_presentation_variants_share_one_identity(self):
        """Comments, key order, alias order and extra-definition order do not move the digest."""
        a = cs.build_schema(cs.load_column_map(DATA_DIR / "presentation_a.yaml"))
        b = cs.build_schema(cs.load_column_map(DATA_DIR / "presentation_b.yaml"))
        self.assertEqual(a.digest, b.digest)

        extras = [scalar_extra(name="Alpha"), scalar_extra(name="Beta", field="num_prog")]
        first = schema_for(valid_profile("consistent_trees_ascii", extras))
        second = schema_for(valid_profile("consistent_trees_ascii", list(reversed(extras))))
        self.assertEqual(first.digest, second.digest)

        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["snap"] = ["snap_idx", "snap_num"]
        forward = schema_for(profile)
        profile["required_columns"]["snap"] = ["snap_num", "snap_idx"]
        backward = schema_for(profile)
        self.assertEqual(forward.digest, backward.digest)

    def test_canonical_json_is_sorted_compact_and_nan_free(self):
        schema = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra()]))
        text = schema.canonical_json()
        self.assertNotIn(", ", text)
        self.assertNotIn(": ", text)
        self.assertNotIn("NaN", text)
        document = json.loads(text)
        self.assertEqual(list(document), sorted(document))

    def test_same_width_type_substitutions_change_the_digest(self):
        """int/float, long long/double and vec3_int/vec3_float share widths."""
        pairs = (("int", "float"), ("long long", "double"), ("vec3_int", "vec3_float"))
        for left, right in pairs:
            with self.subTest(pair=(left, right)):
                self.assertEqual(cs.EXTRA_TYPES[left].itemsize, cs.EXTRA_TYPES[right].itemsize)
                sources = [{"field": "Rvir"}]
                if cs.EXTRA_TYPES[left].n_components == 3:
                    sources = [{"field": "x"}, {"field": "y"}, {"field": "z"}]
                base = {
                    "name": "Selected",
                    "sources": sources,
                    "units": "dimensionless",
                    "h_convention": "none",
                    "description": "test",
                }
                left_extra = dict(base, type=left)
                right_extra = dict(base, type=right)
                left_digest = schema_for(
                    valid_profile("consistent_trees_ascii", [left_extra])
                ).digest
                right_digest = schema_for(
                    valid_profile("consistent_trees_ascii", [right_extra])
                ).digest
                self.assertNotEqual(left_digest, right_digest)

    def test_semantic_changes_move_the_digest(self):
        base_extra = scalar_extra()
        base = schema_for(valid_profile("consistent_trees_ascii", [base_extra])).digest
        mutations = {
            "name": dict(base_extra, name="Renamed"),
            "units": dict(base_extra, units="Mpc/h"),
            "h_convention": dict(base_extra, h_convention="free"),
            "description": dict(base_extra, description="different"),
            "source_field": dict(base_extra, sources=[{"field": "num_prog"}]),
            "source_component": dict(base_extra, sources=[{"field": "Rvir", "component": 1}]),
        }
        for label, extra in mutations.items():
            with self.subTest(change=label):
                self.assertNotEqual(
                    base, schema_for(valid_profile("consistent_trees_ascii", [extra])).digest
                )
        with self.subTest(change="alias_set"):
            profile = valid_profile("consistent_trees_ascii", [base_extra])
            profile["required_columns"]["snap"] = ["snap_num"]
            self.assertNotEqual(base, schema_for(profile).digest)

    def test_source_format_changes_the_digest(self):
        properties = cs.load_source_properties(LHALO_PROPERTIES)
        ascii_digest = schema_for(valid_profile("consistent_trees_ascii")).digest
        hdf5_digest = schema_for(valid_profile("consistent_trees_hdf5")).digest
        binary_digest = schema_for(valid_profile("lhalo_binary"), properties).digest
        self.assertEqual(len({ascii_digest, hdf5_digest, binary_digest}), 3)

    def test_binary_offset_change_moves_the_digest(self):
        properties = cs.load_source_properties(LHALO_PROPERTIES)
        base = schema_for(valid_profile("lhalo_binary"), properties).digest
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["itemsize"] = 112
        profile["binary_layout"]["offsets"]["SubHalfMass"] = 104
        self.assertNotEqual(base, schema_for(profile, properties).digest)

    def test_schema_version_is_the_profile_s_own_parsed_value(self):
        """The digest is durable on-disk provenance, so it must serialize the
        profile's parsed version, never the module constant.

        Today they are always equal, which is exactly why this needs a test:
        the day a version 2 grammar is accepted, serializing the constant
        would silently re-hash every previously frozen version 1 mapping away
        from the value stamped in the files it already produced.
        """
        schema = schema_for(valid_profile("consistent_trees_ascii"))
        self.assertEqual(schema.schema_version, cs.PROFILE_SCHEMA_VERSION)
        self.assertEqual(json.loads(schema.canonical_json())["schema_version"], 1)

        # Constructed directly, bypassing the parser's version check, to prove
        # the serialized value tracks the object rather than the constant.
        hypothetical = dataclasses.replace(schema, schema_version=2)
        self.assertEqual(json.loads(hypothetical.canonical_json())["schema_version"], 2)
        self.assertNotEqual(hypothetical.digest, schema.digest)


class WideIntegerTests(unittest.TestCase):
    """Integers never pass through floating point."""

    #: A synthetic binary source whose properties are exactly the required
    #: roles, so the profile resolves. Every field but the last is packed
    #: contiguously; the last is pushed out to a very large offset.
    WIDE_PROPERTIES = (
        ("Descendant", "int"),
        ("FirstProgenitor", "int"),
        ("NextProgenitor", "int"),
        ("FirstHaloInFOFgroup", "int"),
        ("NextHaloInFOFgroup", "int"),
        ("Len", "int"),
        ("SnapNum", "int"),
        ("M_Crit200", "float"),
        ("VelDisp", "float"),
        ("Vmax", "float"),
        ("MostBoundID", "long long"),
        ("Pos", "vec3_float"),
        ("Vel", "vec3_float"),
        ("Spin", "vec3_float"),
    )

    def _wide_layout_schema(self, far_offset):
        """Schema whose trailing `Spin` field sits at `far_offset`."""
        properties = tuple(
            cs.SourceProperty(name, type_name, "dimensionless")
            for name, type_name in self.WIDE_PROPERTIES
        )
        offsets = {}
        cursor = 0
        for name, type_name in self.WIDE_PROPERTIES[:-1]:
            offsets[name] = cursor
            cursor += cs.EXTRA_TYPES[type_name].itemsize
        last_name, last_type = self.WIDE_PROPERTIES[-1]
        last_width = cs.EXTRA_TYPES[last_type].itemsize
        self.assertGreater(far_offset, cursor, "the far offset must clear the packed prefix")
        offsets[last_name] = far_offset
        profile = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": {role: [role] for role in cs.REQUIRED_ROLES["lhalo_binary"]},
            "extra_fields": [],
            "binary_layout": {
                "byte_order": "little",
                "itemsize": far_offset + last_width,
                "offsets": offsets,
            },
        }
        return schema_for(profile, properties)

    def _far_entry(self, schema):
        document = json.loads(schema.canonical_json())
        entries = document["source_layout"]["entries"]
        return document, next(e for e in entries if e["name"] == "Spin")

    def test_offsets_above_2_31_and_2_53_survive_serialization_exactly(self):
        for offset in (2**31, 2**31 + 1, 2**53, 2**53 + 1, 2**62):
            with self.subTest(offset=offset):
                schema = self._wide_layout_schema(offset)
                document, entry = self._far_entry(schema)
                self.assertEqual(entry["offset"], offset)
                self.assertIsInstance(entry["offset"], int)
                self.assertEqual(document["source_layout"]["itemsize"], offset + 12)

    def test_values_one_apart_above_2_53_are_distinguishable(self):
        """float64 cannot represent 2**53 + 1; the digest must still differ."""
        low = self._wide_layout_schema(2**53)
        high = self._wide_layout_schema(2**53 + 1)
        self.assertNotEqual(low.digest, high.digest)


class AliasResolutionTests(unittest.TestCase):
    ASCII_COLUMNS = [
        "scale(0)",
        "id(1)",
        "desc_scale(2)",
        "desc_id(3)",
        "num_prog(4)",
        "pid(5)",
        "upid(6)",
        "Mvir(10)",
        "Rvir(11)",
        "vrms(13)",
        "vmax(16)",
        "x(17)",
        "y(18)",
        "z(19)",
        "vx(20)",
        "vy(21)",
        "vz(22)",
        "Jx(23)",
        "Jy(24)",
        "Jz(25)",
        "Snap_num(31)",
    ]

    def test_ascii_aliases_match_case_insensitively_and_suffix_stripped(self):
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        resolved = cs.resolve_required_columns(schema, self.ASCII_COLUMNS)
        self.assertEqual(resolved["mvir"], "Mvir(10)")
        self.assertEqual(resolved["snap"], "Snap_num(31)")
        self.assertEqual(set(resolved), set(cs.REQUIRED_ROLES["consistent_trees_ascii"]))

    def test_ascii_exactly_one_snapshot_spelling_must_resolve(self):
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        both = self.ASCII_COLUMNS + ["snap_idx(32)"]
        with self.assertRaisesRegex(ConverterError, "ambiguously"):
            cs.resolve_required_columns(schema, both)
        neither = [c for c in self.ASCII_COLUMNS if not c.startswith("Snap_num")]
        with self.assertRaisesRegex(ConverterError, "none of the aliases"):
            cs.resolve_required_columns(schema, neither)

    def test_duplicate_source_columns_are_rejected(self):
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        with self.assertRaisesRegex(ConverterError, "normalize to the same column"):
            cs.resolve_required_columns(schema, self.ASCII_COLUMNS + ["MVIR(40)"])

    def test_hdf5_aliases_are_matched_exactly(self):
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_hdf5.yaml"))
        exact = list(cs.REQUIRED_ROLES["consistent_trees_hdf5"])
        exact.remove("snap")
        exact.append("Snap_num")
        resolved = cs.resolve_required_columns(schema, exact)
        self.assertEqual(resolved["snap"], "Snap_num")
        wrong_case = [name.lower() if name == "Mvir" else name for name in exact]
        with self.assertRaisesRegex(ConverterError, "none of the aliases"):
            cs.resolve_required_columns(schema, wrong_case)

    def test_two_roles_resolving_to_one_source_field_fail(self):
        """The collision check has to bite in the *shared resolver*.

        `build_schema` can only enforce it for `lhalo_binary`; ASCII and
        forests-HDF5 adapters resolve through `resolve_required_columns`, so an
        adapter following the documented obligation must get the protection
        without having to remember a second call.
        """
        ascii_profile = valid_profile("consistent_trees_ascii")
        ascii_profile["required_columns"]["desc_id"] = ["id"]
        ascii_profile["required_columns"]["snap"] = ["snap_num"]
        hdf5_profile = valid_profile("consistent_trees_hdf5")
        hdf5_profile["required_columns"]["Mvir"] = ["vmax"]
        hdf5_profile["required_columns"]["snap"] = ["Snap_num"]
        hdf5_columns = [
            role for role in cs.REQUIRED_ROLES["consistent_trees_hdf5"] if role != "snap"
        ] + ["Snap_num"]
        for label, profile, available in (
            ("ascii", ascii_profile, self.ASCII_COLUMNS),
            ("hdf5", hdf5_profile, hdf5_columns),
        ):
            with self.subTest(source_format=label):
                schema = schema_for(profile)
                with self.assertRaisesRegex(
                    ConverterError, "both resolve to the same source field"
                ):
                    cs.resolve_required_columns(schema, available)

    def test_a_repeated_identical_column_name_is_left_to_the_adapter(self):
        """`available` is a caller-supplied iterable; a repeated entry in it is
        not evidence that the file carries the column twice. Detecting a
        genuinely duplicated column belongs to the reading adapter."""
        schema = cs.build_schema(cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii.yaml"))
        resolved = cs.resolve_required_columns(schema, self.ASCII_COLUMNS + ["Mvir(10)"])
        self.assertEqual(resolved["mvir"], "Mvir(10)")

    def test_extra_sources_resolve_or_fail_loudly(self):
        schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii_extras_example.yaml")
        )
        resolved = cs.resolve_extra_sources(schema, self.ASCII_COLUMNS)
        self.assertEqual(resolved["CatalogRvir"], (("Rvir(11)", None),))
        self.assertEqual(
            resolved["AngularMomentum"],
            (("Jx(23)", None), ("Jy(24)", None), ("Jz(25)", None)),
        )
        without_rvir = [c for c in self.ASCII_COLUMNS if not c.startswith("Rvir")]
        with self.assertRaisesRegex(ConverterError, "does not carry field"):
            cs.resolve_extra_sources(schema, without_rvir)

    def test_vector_component_survives_extra_resolution(self):
        """`PosX <- Pos component 0` must stay distinguishable from "read the
        whole Pos vector" from the resolver's return value alone."""
        properties = cs.load_source_properties(LHALO_PROPERTIES)
        schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary_extras_example.yaml"), properties
        )
        resolved = cs.resolve_extra_sources(schema, [prop.name for prop in properties])
        self.assertEqual(resolved["PosX"], (("Pos", 0),))
        self.assertEqual(resolved["M_Mean200"], (("M_Mean200", None),))


class V3FieldTableTests(unittest.TestCase):
    """The fixed v3 format table, exactly as the format specifies it."""

    def test_the_fixed_field_tables_are_exactly_the_format_tables(self):
        """Five int64 links and three int32 snapshot columns, three int64 identity arrays, and
        three int64 forest sidecar arrays, in format order."""
        links = [f for f in cs.TOPOLOGY_FIELDS if f.type == "long long"]
        snapshots = [f for f in cs.TOPOLOGY_FIELDS if f.type == "int"]
        self.assertEqual(
            [f.name for f in links],
            [
                "Descendant",
                "FirstProgenitor",
                "NextProgenitor",
                "FirstHaloInFOFgroup",
                "NextHaloInFOFgroup",
            ],
        )
        self.assertEqual(
            [f.name for f in snapshots],
            ["DescendantSnapshot", "FirstProgenitorSnapshot", "NextProgenitorSnapshot"],
        )
        self.assertEqual(
            [(f.name, f.type) for f in cs.IDENTITY_FIELDS],
            [
                ("SourceHaloID", "long long"),
                ("ForestIndex", "long long"),
                ("HaloRankInForest", "long long"),
            ],
        )
        self.assertEqual(
            [(f.name, f.type) for f in cs.FOREST_SIDECAR_FIELDS],
            [
                ("ForestID", "long long"),
                ("SourceFileOrdinal", "long long"),
                ("SourceUnitOrdinal", "long long"),
            ],
        )

    def test_source_halo_id_is_not_most_bound_id(self):
        identity_names = {f.name for f in cs.IDENTITY_FIELDS}
        self.assertIn("SourceHaloID", identity_names)
        self.assertNotIn("MostBoundID", identity_names)
        for source_format in cs.SOURCE_FORMATS:
            payload = {f.name: f for f in cs.PAYLOAD_FIELDS[source_format]}
            self.assertIn("MostBoundID", payload)
            self.assertEqual(payload["MostBoundID"].type, "long long")
            self.assertNotIn("SourceHaloID", payload)

    def test_identity_conventions_are_declared_per_source_format(self):
        shared = {"forest_index", "halo_rank_in_forest", "forest_id", "ordinals"}
        for source_format in cs.SOURCE_FORMATS:
            convention = cs.SOURCE_IDENTITY_CONVENTIONS[source_format]
            # only the ASCII route defines SourceHaloID by its identities
            # rather than by its physical inventory
            extra = {"source_halo_id"} if source_format == "consistent_trees_ascii" else set()
            self.assertEqual(set(convention), shared | extra, source_format)

    def test_mass_keeps_its_native_type_and_units(self):
        """L-Halo mass stays float32 in 1e10 Msun/h; Consistent Trees mass stays Msun/h."""
        payload = {f.name: f for f in cs.PAYLOAD_FIELDS["lhalo_binary"]}
        self.assertEqual(payload["M_Crit200"].type, "float")
        self.assertEqual(payload["M_Crit200"].units, "1e10 Msun/h")
        self.assertEqual(cs.EXTRA_TYPES[payload["M_Crit200"].type].numpy_dtype, "float32")
        for source_format in ("consistent_trees_ascii", "consistent_trees_hdf5"):
            payload = {f.name: f for f in cs.PAYLOAD_FIELDS[source_format]}
            self.assertEqual(payload["M_Crit200"].units, "Msun/h")

    def test_core_payload_names_are_every_format_payload_in_declared_order(self):
        """The reserved core names and each format's payload table are one list.

        ``RESERVED_OUTPUT_NAMES`` is built from ``_CORE_PAYLOAD_NAMES`` before
        the payload tables exist, so a payload field added to or renamed in one
        table without the other would leave an extra free to shadow it.
        """
        for source_format in cs.SOURCE_FORMATS:
            with self.subTest(source_format=source_format):
                self.assertEqual(
                    tuple(field.name for field in cs.PAYLOAD_FIELDS[source_format]),
                    cs._CORE_PAYLOAD_NAMES,
                )

    def test_output_declarations_cover_payload_and_extras_only(self):
        schema = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra(name="Rvir")]))
        declared = {field.name for field in schema.output_field_declarations()}
        for required in ("Len", "SnapNum", "MostBoundID", "Rvir"):
            self.assertIn(required, declared)
        for governed in ("Descendant", "SourceHaloID", "ForestIndex", "DescendantSnapshot"):
            self.assertNotIn(governed, declared)


class PropertyGeneratorVocabularyTests(unittest.TestCase):
    """The emitted schema must speak the property generator's vocabulary.

    Checked mechanically against scripts/generate_properties.py itself rather
    than against a copy of its tables.
    """

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REPO_ROOT / "scripts"))
        import generate_properties  # noqa: E402

        cls.generator = generate_properties

    def test_declarable_types_are_exactly_the_generator_numeric_types(self):
        self.assertEqual(set(cs.EXTRA_TYPES), set(self.generator.TYPE_MAP))

    def test_declared_widths_and_shapes_match_the_generator(self):
        for type_name, spec in cs.EXTRA_TYPES.items():
            with self.subTest(type=type_name):
                generated = self.generator.TYPE_MAP[type_name]
                self.assertEqual(generated["is_array"], spec.n_components == 3)
                if spec.n_components == 3:
                    self.assertEqual(generated["array_size"], 3)
                    self.assertEqual(generated["numpy_type"], "(np.{}, 3)".format(spec.numpy_dtype))
                else:
                    self.assertEqual(generated["numpy_type"], "np." + spec.numpy_dtype)

    def test_h_conventions_match_the_generator(self):
        self.assertEqual(set(cs.H_CONVENTIONS), set(self.generator.H_CONVENTIONS))

    def test_payload_units_are_in_the_generator_registry(self):
        for source_format in cs.SOURCE_FORMATS:
            for field in cs.PAYLOAD_FIELDS[source_format]:
                with self.subTest(source_format=source_format, field=field.name):
                    self.assertIn(field.units, self.generator.UNIT_REGISTRY)
                    self.assertEqual(
                        field.h_convention,
                        self.generator.UNIT_REGISTRY[field.units]["h_convention"],
                    )


class SourceReaderVocabularyTests(unittest.TestCase):
    """Each format's required roles must be the names its reference reader reads.

    Checked against the readers themselves -- the ASCII parser's column tables
    and the C forests-HDF5 reader's field table -- rather than against a copy,
    so a column added to or dropped from a reader fails here instead of
    surfacing as an unmappable role at conversion time.
    """

    def test_ascii_roles_are_the_parser_required_columns_plus_snap(self):
        roles = cs.REQUIRED_ROLES["consistent_trees_ascii"]
        self.assertEqual(len(roles), len(set(roles)))
        self.assertEqual(
            set(roles),
            set(ctrees_parser._INT_COLUMNS) | set(ctrees_parser._FLOAT_COLUMNS) | {"snap"},
        )

    def test_hdf5_roles_are_the_c_reader_fixed_fields_plus_snap(self):
        """The C table names every field but the snapshot, whose dataset name
        (``Snap_num`` or ``Snap_idx``) the reader chooses per file at runtime and
        which the profile therefore maps through the ``snap`` role."""
        source = CTREES_HDF5_READER.read_text()
        table = re.search(
            r"CTREES_H5_FIXED_FIELD_NAMES\[CTREES_H5_FIELD_COUNT\]\s*=\s*\{(.*?)\};",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(table, "CTREES_H5_FIXED_FIELD_NAMES not found in the C reader")
        entries = re.findall(r'\[(\w+)\]\s*=\s*(?:"([^"]*)"|(NULL))', table.group(1))
        self.assertTrue(entries, "CTREES_H5_FIXED_FIELD_NAMES has no parsable entries")
        named = [name for _slot, name, null in entries if not null]
        runtime_named = [slot for slot, _name, null in entries if null]
        self.assertEqual(runtime_named, ["CTREES_H5_FIELD_SNAP"])
        self.assertEqual(len(named), len(set(named)))

        roles = cs.REQUIRED_ROLES["consistent_trees_hdf5"]
        self.assertEqual(len(roles), len(set(roles)))
        self.assertEqual(set(roles), set(named) | {"snap"})
