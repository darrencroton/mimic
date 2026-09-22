"""Slice 2 unit tests: canonical source schema, mapping profiles and schema
identity (scripts/convert/column_schema.py).

Three properties carry most of the weight here and are tested directly rather
than inferred:

- **Presentation does not move the digest.** Comments, key order, alias order
  and extra-definition order are normalised away; anything that changes how a
  value is read, typed, named or labelled is not.
- **Same-width substitutions are visible.** ``int``/``float``,
  ``long long``/``double`` and ``vec3_int``/``vec3_float`` occupy the same
  bytes and must still produce different digests, because Slice 7's resume
  path must reject a same-width schema substitution before it mutates
  anything.
- **Integers never pass through floating point.** Offsets, itemsizes and
  declared values above 2**31 and 2**53 survive canonical serialization
  exactly, and 2**53 and 2**53 + 1 are distinguishable.

The shipped profiles under scripts/convert/profiles/ are loaded on every run,
so documentation that stops being runnable fails here rather than in a later
conversion.
"""

import copy
import dataclasses
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import column_schema as cs  # noqa: E402
from ctrees_parser import ConverterError  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
PROFILE_DIR = REPO_ROOT / "scripts" / "convert" / "profiles"
DATA_DIR = Path(__file__).parent / "data" / "column_maps"
LHALO_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"


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
        self.assertTrue(paths, "no profiles found under scripts/convert/profiles/")
        for path in paths:
            with self.subTest(profile=path.name):
                column_map = cs.load_column_map(path)
                properties = (
                    self.source_properties if column_map.source_format == "lhalo_binary" else None
                )
                schema = cs.build_schema(column_map, properties)
                self.assertEqual(len(schema.digest), 64)
                self.assertEqual(schema.digest, schema.digest.lower())

    def test_shipped_profiles_have_distinct_identities(self):
        digests = {}
        for path in sorted(PROFILE_DIR.glob("*.yaml")):
            column_map = cs.load_column_map(path)
            properties = (
                self.source_properties if column_map.source_format == "lhalo_binary" else None
            )
            digests[path.name] = cs.build_schema(column_map, properties).digest
        self.assertEqual(len(set(digests.values())), len(digests), digests)

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

    def test_unselected_source_fields_stay_in_the_layout(self):
        """Extra selection must not alter the on-disk stride (C2)."""
        schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary.yaml"), self.source_properties
        )
        names = [entry.name for entry in schema.source_layout.entries]
        for unselected in ("M_Mean200", "M_TopHat", "FileNr", "SubhaloIndex", "SubHalfMass"):
            self.assertIn(unselected, names)
            entry = next(e for e in schema.source_layout.entries if e.name == unselected)
            self.assertFalse(entry.selected)
        self.assertEqual(schema.source_layout.itemsize, 104)

    def test_selecting_an_extra_marks_it_selected_without_changing_the_stride(self):
        default = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary.yaml"), self.source_properties
        )
        with_extras = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "lhalo_binary_extras_example.yaml"),
            self.source_properties,
        )
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

    def test_unknown_top_level_key_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["column_map_version"] = 1
        with self.assertRaisesRegex(ConverterError, "unknown key"):
            schema_for(profile)

    def test_missing_top_level_key_fails(self):
        for key in ("schema_version", "required_columns", "extra_fields"):
            with self.subTest(key=key):
                profile = valid_profile("consistent_trees_ascii")
                del profile[key]
                with self.assertRaises(ConverterError):
                    schema_for(profile)

    def test_wrong_schema_version_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["schema_version"] = 2
        with self.assertRaisesRegex(ConverterError, "schema_version"):
            schema_for(profile)

    def test_unknown_source_format_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["source_format"] = "lhalo_hdf5"
        with self.assertRaisesRegex(ConverterError, "source_format"):
            schema_for(profile)

    def test_missing_required_role_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        del profile["required_columns"]["snap"]
        with self.assertRaisesRegex(ConverterError, "missing required key"):
            schema_for(profile)

    def test_unknown_required_role_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["rvir"] = ["Rvir"]
        with self.assertRaisesRegex(ConverterError, "unknown key"):
            schema_for(profile)

    def test_empty_alias_list_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["snap"] = []
        with self.assertRaisesRegex(ConverterError, "empty alias list"):
            schema_for(profile)

    def test_bare_string_alias_list_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["snap"] = "snap_num"
        with self.assertRaisesRegex(ConverterError, "list of aliases"):
            schema_for(profile)

    def test_aliases_that_normalize_to_the_same_name_fail(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["mvir"] = ["Mvir", "mvir(10)"]
        with self.assertRaisesRegex(ConverterError, "twice after normalization"):
            schema_for(profile)

    def test_structural_metadata_is_not_a_remappable_role(self):
        """Tree headers and ForestInfo are adapter-owned (C2)."""
        self.assertNotIn("ForestInfo", cs.REQUIRED_ROLES["consistent_trees_hdf5"])
        self.assertNotIn("Ntrees", cs.REQUIRED_ROLES["lhalo_binary"])


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

    def test_unsupported_type_fails(self):
        profile = valid_profile("consistent_trees_ascii", [scalar_extra(type_name="float16")])
        with self.assertRaisesRegex(ConverterError, "unsupported type"):
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

    def test_invalid_component_index_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["extra_fields"] = [
            {
                "name": "Selected",
                "sources": [{"field": "Pos", "component": 3}],
                "type": "float",
                "units": "Mpc/h",
                "h_convention": "carried",
                "description": "test",
            }
        ]
        with self.assertRaisesRegex(ConverterError, "component must be 0, 1 or 2"):
            schema_for(profile)

    def test_float_component_index_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["extra_fields"] = [
            {
                "name": "Selected",
                "sources": [{"field": "Pos", "component": 1.0}],
                "type": "float",
                "units": "Mpc/h",
                "h_convention": "carried",
                "description": "test",
            }
        ]
        with self.assertRaisesRegex(ConverterError, "must be an integer"):
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

    def test_unknown_source_component_key_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["extra_fields"] = [
            {
                "name": "Selected",
                "sources": [{"field": "Rvir", "index": 0}],
                "type": "float",
                "units": "kpc/h",
                "h_convention": "carried",
                "description": "test",
            }
        ]
        with self.assertRaisesRegex(ConverterError, "unknown source key"):
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
        for name in ("0Extra", "extra-field", "extra field", "_Extra", "Exträ"):
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

    def test_invalid_h_convention_is_rejected(self):
        profile = valid_profile("consistent_trees_ascii", [scalar_extra(convention="comoving")])
        with self.assertRaisesRegex(ConverterError, "h_convention"):
            schema_for(profile)

    def test_empty_units_and_description_are_rejected(self):
        for key in ("units", "description"):
            with self.subTest(key=key):
                extra = scalar_extra()
                extra[key] = "  "
                profile = valid_profile("consistent_trees_ascii", [extra])
                with self.assertRaisesRegex(ConverterError, "nonempty string"):
                    schema_for(profile)

    def test_missing_extra_key_is_rejected(self):
        for key in cs._EXTRA_ENTRY_KEYS:
            with self.subTest(key=key):
                extra = scalar_extra()
                del extra[key]
                profile = valid_profile("consistent_trees_ascii", [extra])
                with self.assertRaises(ConverterError):
                    schema_for(profile)

    def test_unknown_extra_key_is_rejected(self):
        extra = scalar_extra()
        extra["output_convert"] = "scale"
        profile = valid_profile("consistent_trees_ascii", [extra])
        with self.assertRaisesRegex(ConverterError, "unknown key"):
            schema_for(profile)

    def test_null_extra_fields_is_rejected(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["extra_fields"] = None
        with self.assertRaisesRegex(ConverterError, "must be a list"):
            schema_for(profile)


class BinaryLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_properties = cs.load_source_properties(LHALO_PROPERTIES)

    def test_binary_layout_on_a_non_binary_profile_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["binary_layout"] = {"byte_order": "little", "itemsize": 104, "offsets": {"x": 0}}
        with self.assertRaisesRegex(ConverterError, "unknown key"):
            schema_for(profile)

    def test_missing_binary_layout_on_a_binary_profile_fails(self):
        profile = valid_profile("lhalo_binary")
        del profile["binary_layout"]
        with self.assertRaisesRegex(ConverterError, "missing required key"):
            schema_for(profile, self.source_properties)

    def test_binary_profile_without_source_properties_fails(self):
        profile = valid_profile("lhalo_binary")
        with self.assertRaisesRegex(ConverterError, "halo_properties.yaml"):
            schema_for(profile, None)

    def test_source_properties_on_a_non_binary_profile_fails(self):
        profile = valid_profile("consistent_trees_ascii")
        with self.assertRaisesRegex(ConverterError, "lhalo_binary profiles only"):
            schema_for(profile, self.source_properties)

    def test_invalid_byte_order_fails(self):
        for byte_order in ("native", "=", "<", None):
            with self.subTest(byte_order=byte_order):
                profile = valid_profile("lhalo_binary")
                profile["binary_layout"]["byte_order"] = byte_order
                with self.assertRaisesRegex(ConverterError, "byte_order"):
                    schema_for(profile, self.source_properties)

    def test_big_endian_is_supported_and_changes_the_identity(self):
        little = schema_for(valid_profile("lhalo_binary"), self.source_properties)
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["byte_order"] = "big"
        big = schema_for(profile, self.source_properties)
        self.assertEqual(big.source_layout.numpy_byte_order, ">")
        self.assertNotEqual(little.digest, big.digest)

    def test_non_integer_itemsize_and_offsets_fail(self):
        for mutate in (
            lambda layout: layout.__setitem__("itemsize", 104.0),
            lambda layout: layout.__setitem__("itemsize", True),
            lambda layout: layout.__setitem__("itemsize", 0),
            lambda layout: layout["offsets"].__setitem__("Descendant", 0.0),
            lambda layout: layout["offsets"].__setitem__("Descendant", -4),
        ):
            with self.subTest(mutate=mutate):
                profile = valid_profile("lhalo_binary")
                mutate(profile["binary_layout"])
                with self.assertRaises(ConverterError):
                    schema_for(profile, self.source_properties)

    def test_padding_between_fields_is_allowed(self):
        """A record may carry bytes this converter never reads."""
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["itemsize"] = 112
        profile["binary_layout"]["offsets"]["SubHalfMass"] = 108
        schema = schema_for(profile, self.source_properties)
        self.assertEqual(schema.source_layout.itemsize, 112)

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

    def test_numpy_dtype_spec_preserves_padding(self):
        """A padded record must not be read with a packed stride."""
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["itemsize"] = 128
        schema = schema_for(profile, self.source_properties)
        dtype = np.dtype(schema.source_layout.numpy_dtype_spec())
        self.assertEqual(dtype.itemsize, 128)

    def test_big_endian_dtype_spec_uses_the_declared_order(self):
        profile = valid_profile("lhalo_binary")
        profile["binary_layout"]["byte_order"] = "big"
        schema = schema_for(profile, self.source_properties)
        dtype = np.dtype(schema.source_layout.numpy_dtype_spec())
        self.assertEqual(dtype["Descendant"].str, ">i4")
        self.assertEqual(dtype["MostBoundID"].str, ">i8")


class YamlLoadingTests(unittest.TestCase):
    def test_duplicate_yaml_keys_fail(self):
        for name in ("duplicate_top_level_key.yaml", "duplicate_extra_key.yaml"):
            with self.subTest(fixture=name):
                with self.assertRaisesRegex(ConverterError, "duplicate key"):
                    cs.load_column_map(DATA_DIR / name)

    def test_malformed_yaml_fails_as_a_converter_error(self):
        with self.assertRaisesRegex(ConverterError, "invalid YAML"):
            cs.load_column_map(DATA_DIR / "malformed.yaml")

    def test_non_mapping_profile_fails(self):
        with self.assertRaisesRegex(ConverterError, "must be a YAML mapping"):
            cs.load_column_map(DATA_DIR / "not_a_mapping.yaml")

    def test_missing_profile_file_fails(self):
        with tempfile.TemporaryDirectory() as workdir:
            with self.assertRaisesRegex(ConverterError, "cannot read"):
                cs.load_column_map(Path(workdir) / "absent.yaml")


class SchemaIdentityTests(unittest.TestCase):
    def test_presentation_variants_share_one_identity(self):
        a = cs.build_schema(cs.load_column_map(DATA_DIR / "presentation_a.yaml"))
        b = cs.build_schema(cs.load_column_map(DATA_DIR / "presentation_b.yaml"))
        self.assertEqual(a.digest, b.digest)

    def test_reordering_in_memory_does_not_move_the_digest(self):
        extras = [scalar_extra(name="Alpha"), scalar_extra(name="Beta", field="num_prog")]
        first = schema_for(valid_profile("consistent_trees_ascii", extras))
        second = schema_for(valid_profile("consistent_trees_ascii", list(reversed(extras))))
        self.assertEqual(first.digest, second.digest)

    def test_alias_order_does_not_move_the_digest(self):
        profile = valid_profile("consistent_trees_ascii")
        profile["required_columns"]["snap"] = ["snap_idx", "snap_num"]
        forward = schema_for(profile)
        profile["required_columns"]["snap"] = ["snap_num", "snap_idx"]
        backward = schema_for(profile)
        self.assertEqual(forward.digest, backward.digest)

    def test_digest_is_deterministic_across_calls(self):
        schema = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra()]))
        self.assertEqual(schema.digest, schema.digest)
        rebuilt = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra()]))
        self.assertEqual(schema.digest, rebuilt.digest)

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

    def test_alias_set_change_moves_the_digest(self):
        base = schema_for(valid_profile("consistent_trees_ascii")).digest
        profile = valid_profile("consistent_trees_ascii")
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


class WideIntegerTests(unittest.TestCase):
    """Integers never pass through floating point (C2)."""

    def _wide_layout_schema(self, itemsize, offset):
        properties = (cs.SourceProperty("Only", "long long", "dimensionless"),)
        profile = {
            "schema_version": 1,
            "source_format": "lhalo_binary",
            "required_columns": {role: [role] for role in cs.REQUIRED_ROLES["lhalo_binary"]},
            "extra_fields": [],
            "binary_layout": {
                "byte_order": "little",
                "itemsize": itemsize,
                "offsets": {"Only": offset},
            },
        }
        return schema_for(profile, properties)

    def test_offsets_above_2_31_and_2_53_survive_serialization_exactly(self):
        for offset in (2**31, 2**31 + 1, 2**53, 2**53 + 1, 2**62):
            with self.subTest(offset=offset):
                schema = self._wide_layout_schema(offset + 8, offset)
                document = json.loads(schema.canonical_json())
                entry = document["source_layout"]["entries"][0]
                self.assertEqual(entry["offset"], offset)
                self.assertIsInstance(entry["offset"], int)
                self.assertEqual(document["source_layout"]["itemsize"], offset + 8)

    def test_values_one_apart_above_2_53_are_distinguishable(self):
        """float64 cannot represent 2**53 + 1; the digest must still differ."""
        low = self._wide_layout_schema(2**53 + 8, 2**53)
        high = self._wide_layout_schema(2**53 + 9, 2**53 + 1)
        self.assertNotEqual(low.digest, high.digest)

    def test_canonical_json_text_carries_the_exact_digits(self):
        schema = self._wide_layout_schema(2**53 + 9, 2**53 + 1)
        self.assertIn(str(2**53 + 1), schema.canonical_json())


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

    def test_extra_sources_resolve_or_fail_loudly(self):
        schema = cs.build_schema(
            cs.load_column_map(PROFILE_DIR / "consistent_trees_ascii_extras_example.yaml")
        )
        resolved = cs.resolve_extra_sources(schema, self.ASCII_COLUMNS)
        self.assertEqual(resolved["Rvir"], ("Rvir(11)",))
        self.assertEqual(resolved["AngularMomentum"], ("Jx(23)", "Jy(24)", "Jz(25)"))
        without_rvir = [c for c in self.ASCII_COLUMNS if not c.startswith("Rvir")]
        with self.assertRaisesRegex(ConverterError, "does not carry field"):
            cs.resolve_extra_sources(schema, without_rvir)


class V3FieldTableTests(unittest.TestCase):
    """The fixed v3 format table, exactly as C3 specifies it."""

    def test_five_int64_links_and_three_int32_snapshot_columns(self):
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

    def test_three_int64_identity_arrays(self):
        self.assertEqual(
            [(f.name, f.type) for f in cs.IDENTITY_FIELDS],
            [
                ("SourceHaloID", "long long"),
                ("ForestIndex", "long long"),
                ("HaloRankInForest", "long long"),
            ],
        )

    def test_forest_sidecar_carries_three_int64_arrays(self):
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
        for source_format in cs.SOURCE_FORMATS:
            convention = cs.SOURCE_IDENTITY_CONVENTIONS[source_format]
            self.assertEqual(
                set(convention),
                {"forest_index", "halo_rank_in_forest", "forest_id", "ordinals"},
            )

    def test_lhalo_mass_stays_float32_in_1e10_msun_h(self):
        payload = {f.name: f for f in cs.PAYLOAD_FIELDS["lhalo_binary"]}
        self.assertEqual(payload["M_Crit200"].type, "float")
        self.assertEqual(payload["M_Crit200"].units, "1e10 Msun/h")
        self.assertEqual(cs.EXTRA_TYPES[payload["M_Crit200"].type].numpy_dtype, "float32")

    def test_ctrees_mass_stays_native_msun_h(self):
        for source_format in ("consistent_trees_ascii", "consistent_trees_hdf5"):
            payload = {f.name: f for f in cs.PAYLOAD_FIELDS[source_format]}
            self.assertEqual(payload["M_Crit200"].units, "Msun/h")

    def test_output_declarations_cover_payload_and_extras_only(self):
        schema = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra(name="Rvir")]))
        declared = {field.name for field in schema.output_field_declarations()}
        for required in ("Len", "SnapNum", "MostBoundID", "Rvir"):
            self.assertIn(required, declared)
        for governed in ("Descendant", "SourceHaloID", "ForestIndex", "DescendantSnapshot"):
            self.assertNotIn(governed, declared)

    def test_consumer_metadata_fragment_is_marked_incomplete(self):
        schema = schema_for(
            valid_profile("lhalo_binary"), cs.load_source_properties(LHALO_PROPERTIES)
        )
        fragment = schema.consumer_metadata_fragment()
        self.assertFalse(fragment["complete"])
        self.assertIn("runtime topology support", fragment["incomplete_because"])
        self.assertEqual(fragment["column_mapping_sha256"], schema.digest)
        bindings = {
            entry["name"]: entry["provides_core_role"] for entry in fragment["halo_properties"]
        }
        self.assertEqual(bindings["M_Crit200"], "HaloMass")
        self.assertEqual(bindings["Len"], "Len")
        self.assertIsNone(bindings["MostBoundID"])
        table = {entry["name"] for entry in fragment["format_table_fields"]}
        self.assertIn("Descendant", table)
        self.assertIn("SourceHaloID", table)


class PropertyGeneratorVocabularyTests(unittest.TestCase):
    """The emitted schema must speak the property generator's vocabulary.

    C3 requires the consumer metadata fragment to be checkable against the
    property generator; this test does that check mechanically against
    scripts/generate_properties.py itself rather than against a copy of its
    tables.
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

    def test_core_role_bindings_are_declared_core_roles(self):
        core_yaml = REPO_ROOT / "src" / "core" / "core_properties.yaml"
        text = yaml.safe_load(core_yaml.read_text())
        required = {entry["name"] for entry in text.get("required_inputs", [])}
        self.assertTrue(required, "core_properties.yaml declares no required_inputs")
        self.assertEqual(set(cs._CORE_ROLE_BINDINGS.values()), required)


class ImmutabilityTests(unittest.TestCase):
    def test_schema_objects_are_frozen(self):
        schema = schema_for(valid_profile("consistent_trees_ascii", [scalar_extra()]))
        with self.assertRaises(dataclasses.FrozenInstanceError):
            schema.source_format = "lhalo_binary"
        with self.assertRaises(dataclasses.FrozenInstanceError):
            schema.extra_fields[0].units = "Mpc/h"
        with self.assertRaises(dataclasses.FrozenInstanceError):
            schema.extra_fields[0].sources[0].component = 2

    def test_parsing_does_not_mutate_the_caller_dict(self):
        profile = valid_profile("consistent_trees_ascii", [scalar_extra()])
        before = copy.deepcopy(profile)
        schema_for(profile)
        self.assertEqual(profile, before)


if __name__ == "__main__":
    unittest.main()
