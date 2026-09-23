"""Slice 2 unit tests: frozen dtype, header dialects, #tree attribution,
independent pre-count, malformed-input aborts.

Converter generalisation Slice 5 adds the schema-driven selection, the
extended scratch layout, declared-extra parsing and source-unit planning
(``Test*Selection*``, ``TestExtraParsing``, ``TestSourceUnits``,
``TestScratchLayout``). Their oracles are literal: expected values are written
out or computed from the fixture's own source text, never from the parser."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from column_schema import build_schema, load_column_map, parse_column_map  # noqa: E402
from ctrees_parser import (  # noqa: E402
    DTYPE_TAG,
    LEGACY_LAYOUT,
    RECORD_DTYPE,
    ConverterError,
    CtreesFileParser,
    ScratchLayout,
    parse_file,
    parse_header_line,
    plan_source_units,
    prescan_file,
    resolve_columns,
    resolve_selection,
)

DATA_DIR = Path(__file__).parent / "data"
PROFILE_DIR = Path(__file__).resolve().parents[1] / "profiles"
DEFAULT_ASCII_PROFILE = PROFILE_DIR / "consistent_trees_ascii.yaml"
ALL_TYPES_PROFILE = DATA_DIR / "column_maps" / "ascii_all_extra_types.yaml"


def ascii_schema(extra_fields=(), **role_overrides):
    """A consistent_trees_ascii schema: the shipped default roles (optionally
    overridden) plus the given extra-field entries, parsed exactly as a
    profile file would be."""
    with open(DEFAULT_ASCII_PROFILE) as handle:
        data = yaml.safe_load(handle)
    data["required_columns"].update(role_overrides)
    data["extra_fields"] = list(extra_fields)
    return build_schema(parse_column_map(data, "<test profile>"))


def extra(name, fields, type_name, component=None):
    sources = [{"field": f} for f in fields]
    if component is not None:
        sources[0]["component"] = component
    return {
        "name": name,
        "sources": sources,
        "type": type_name,
        "units": "dimensionless",
        "h_convention": "none",
        "description": "test extra {}".format(name),
    }


def f32(text):
    """The frozen numeric parse path: float64 text parse, float32 cast."""
    return np.float64(text).astype(np.float32)


class TestFrozenDtype(unittest.TestCase):
    def test_itemsize_is_108_packed_little_endian(self):
        self.assertEqual(RECORD_DTYPE.itemsize, 108)
        self.assertFalse(RECORD_DTYPE.isalignedstruct)

    def test_field_layout_frozen(self):
        expected = [
            ("id", "<i8"),
            ("desc_id", "<i8"),
            ("desc_scale", "<f8"),
            ("pid", "<i8"),
            ("upid", "<i8"),
            ("snap", "<i4"),
            ("Mvir", "<f4"),
            ("X", "<f4"),
            ("Y", "<f4"),
            ("Z", "<f4"),
            ("VX", "<f4"),
            ("VY", "<f4"),
            ("VZ", "<f4"),
            ("Jx", "<f4"),
            ("Jy", "<f4"),
            ("Jz", "<f4"),
            ("vrms", "<f4"),
            ("vmax", "<f4"),
            ("tree_root_id", "<i8"),
            ("forest_id", "<i8"),
        ]
        self.assertEqual(RECORD_DTYPE.descr, expected)
        self.assertIn("itemsize=108", DTYPE_TAG)


class TestHeaderParsing(unittest.TestCase):
    def test_indexed_suffixes_stripped(self):
        names = parse_header_line("#scale(0) id(1) Snap_num(31)")
        self.assertEqual(names, ["scale", "id", "Snap_num"])

    def test_fields_dialect(self):
        names = parse_header_line("#fields: scale id snap_idx")
        self.assertEqual(names, ["scale", "id", "snap_idx"])

    def test_comma_delimiters(self):
        names = parse_header_line("#scale(0),id(1),snap_num(2)")
        self.assertEqual(names, ["scale", "id", "snap_num"])

    def test_non_hash_header_aborts(self):
        with self.assertRaises(ConverterError):
            parse_header_line("scale id snap_num")

    def test_case_insensitive_resolution(self):
        header = (
            "#SCALE(0) ID(1) DESC_SCALE(2) Desc_ID(3) PID(4) UPID(5) MVIR(6) VRMS(7) "
            "VMAX(8) X(9) Y(10) Z(11) VX(12) VY(13) VZ(14) JX(15) JY(16) JZ(17) SNAP_NUM(18)"
        )
        layout = resolve_columns(parse_header_line(header))
        self.assertEqual(layout.snapshot_column, "snap_num")
        self.assertEqual(layout.indices["mvir"], 6)

    def test_duplicate_required_column_aborts(self):
        header = "#scale(0) id(1) id(2) snap_num(3)"
        with self.assertRaisesRegex(ConverterError, "duplicate"):
            resolve_columns(parse_header_line(header))

    def test_both_snapshot_spellings_abort(self):
        names = parse_header_line(fixtures.header_line())
        names.append("snap_idx")
        with self.assertRaisesRegex(ConverterError, "ambiguous snapshot"):
            resolve_columns(names)

    def test_missing_column_aborts(self):
        with self.assertRaisesRegex(ConverterError, "missing required"):
            resolve_columns(["scale", "id", "snap_num"])

    def test_missing_snapshot_column_aborts(self):
        names = [n for n in parse_header_line(fixtures.header_line()) if n != "Snap_num"]
        with self.assertRaisesRegex(ConverterError, "snap_idx/snap_num"):
            resolve_columns(names)


class TestGoldenFixtures(unittest.TestCase):
    def _check_common_records(self, records):
        self.assertEqual(records.dtype, RECORD_DTYPE)
        np.testing.assert_array_equal(records["id"], [1000, 1001, 2000])
        np.testing.assert_array_equal(records["tree_root_id"], [1000, 1000, 2000])
        np.testing.assert_array_equal(records["forest_id"], [-1, -1, -1])
        np.testing.assert_array_equal(records["snap"], [5, 4, 5])
        np.testing.assert_array_equal(records["desc_id"], [-1, 1000, -1])
        np.testing.assert_array_equal(records["desc_scale"], [-1.0, 1.0, -1.0])
        self.assertEqual(records["Mvir"][0], f32("1.5e12"))
        self.assertEqual(records["X"][1], f32("10.1"))
        self.assertEqual(records["Jz"][2], f32("-3e9"))
        self.assertEqual(records["vrms"][0], f32("120.5"))
        self.assertEqual(records["vmax"][0], f32("250.25"))

    def test_indexed_header_golden(self):
        records, result, prescan = parse_file(DATA_DIR / "indexed_header.dat")
        self._check_common_records(records)
        self.assertEqual(prescan.n_rows, 3)
        self.assertEqual(prescan.declared_tree_count, 2)
        self.assertTrue(result.complete)
        self.assertEqual(result.observed_pairs, {(5, 1.0), (4, 0.9)})

    def test_fields_header_golden(self):
        records, result, _ = parse_file(DATA_DIR / "fields_header.dat")
        self._check_common_records(records)
        self.assertTrue(result.complete)

    def test_casing_variants_golden(self):
        records, _, _ = parse_file(DATA_DIR / "casing_variants.dat")
        self.assertEqual(len(records), 1)
        self.assertEqual(records["id"][0], 1000)
        self.assertEqual(records["snap"][0], 5)

    def test_duplicate_column_golden_aborts(self):
        with self.assertRaisesRegex(ConverterError, "duplicate"):
            parse_file(DATA_DIR / "duplicate_column.dat")

    def test_malformed_row_golden_aborts(self):
        with self.assertRaisesRegex(ConverterError, "malformed"):
            parse_file(DATA_DIR / "malformed_row.dat")

    def test_truncated_row_golden_aborts(self):
        with self.assertRaises(ConverterError):
            parse_file(DATA_DIR / "truncated_row.dat")

    def test_tree_boundaries_across_chunks(self):
        expected_roots = np.array([10, 20, 20, 30, 30, 30], dtype=np.int64)
        expected_ids = np.array([10, 20, 21, 30, 31, 32], dtype=np.int64)
        for chunksize in (1, 2, 3, 100):
            records, _, _ = parse_file(DATA_DIR / "tree_boundaries.dat", chunksize=chunksize)
            np.testing.assert_array_equal(records["tree_root_id"], expected_roots)
            np.testing.assert_array_equal(records["id"], expected_ids)


class TestSyntheticFixtures(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_generator_round_trip_both_dialects(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        n_halos = sum(len(t.halos) for t in trees)
        for dialect, snapcol in (("indexed", "Snap_num"), ("fields", "snap_idx")):
            path = fixtures.write_ctrees_file(
                self.dir / "trees_{}.dat".format(dialect),
                trees,
                dialect=dialect,
                snapshot_column=snapcol,
            )
            records, result, prescan = parse_file(path)
            self.assertEqual(len(records), n_halos)
            self.assertEqual(prescan.n_rows, n_halos)
            self.assertTrue(result.complete)
            for tree in trees:
                mask = records["tree_root_id"] == tree.root_id
                self.assertEqual(int(mask.sum()), len(tree.halos))
                np.testing.assert_array_equal(
                    np.sort(records["id"][mask]),
                    np.sort(np.array([h.halo_id for h in tree.halos], dtype=np.int64)),
                )

    def _write_with_token(self, name, column, token):
        """One standard file plus one row whose <column> carries <token>."""
        trees = [
            fixtures.TreeSpec(root_id=9, halos=[fixtures.HaloSpec(halo_id=9, snap=5, mvir=1.0e11)])
        ]
        path = fixtures.write_ctrees_file(self.dir / name, trees)
        lines = path.read_text().splitlines()
        col_index = fixtures.COLUMNS.index(column)
        tokens = lines[-1].split()
        tokens[col_index] = token
        lines[-1] = " ".join(tokens)
        path.write_text("\n".join(lines) + "\n")
        return path

    def test_nan_value_aborts(self):
        path = self._write_with_token("nan.dat", "Mvir", "nan")
        with self.assertRaisesRegex(ConverterError, "non-finite"):
            parse_file(path)

    def test_inf_value_aborts(self):
        path = self._write_with_token("inf.dat", "x", "inf")
        with self.assertRaisesRegex(ConverterError, "non-finite"):
            parse_file(path)

    def test_negative_inf_value_aborts(self):
        path = self._write_with_token("ninf.dat", "vy", "-inf")
        with self.assertRaisesRegex(ConverterError, "non-finite"):
            parse_file(path)

    def test_float32_overflow_aborts(self):
        # finite in float64, infinite after the float32 cast
        path = self._write_with_token("overflow.dat", "Jz", "1e39")
        with self.assertRaisesRegex(ConverterError, "float32-overflowing"):
            parse_file(path)

    def test_nan_scale_aborts(self):
        path = self._write_with_token("nanscale.dat", "scale", "nan")
        with self.assertRaisesRegex(ConverterError, "non-finite"):
            parse_file(path)

    def test_extra_token_row_aborts_in_prescan(self):
        path = self._write_with_token("extra.dat", "Tree_root_ID", "9 42")
        with self.assertRaisesRegex(ConverterError, "token"):
            prescan_file(path)

    def test_missing_ignored_trailing_token_aborts_in_prescan(self):
        # drop the last (converter-ignored) column: still structurally malformed
        trees = [
            fixtures.TreeSpec(root_id=9, halos=[fixtures.HaloSpec(halo_id=9, snap=5, mvir=1.0e11)])
        ]
        path = fixtures.write_ctrees_file(self.dir / "short.dat", trees)
        lines = path.read_text().splitlines()
        lines[-1] = " ".join(lines[-1].split()[:-1])
        path.write_text("\n".join(lines) + "\n")
        with self.assertRaisesRegex(ConverterError, "token"):
            prescan_file(path)

    def test_adversarial_decimal_casts_match_reference_path(self):
        for token in (
            "1.0000001e10",
            "3.14159265358979e-7",
            "123456789.987654321",
            "-9.999999e-38",
        ):
            path = self._write_with_token("adv.dat", "Mvir", token)
            records, _, _ = parse_file(path)
            self.assertEqual(records["Mvir"][-1], f32(token))

    def test_float32_cast_matches_reference_path(self):
        trees = [
            fixtures.TreeSpec(
                root_id=7, halos=[fixtures.HaloSpec(halo_id=7, snap=5, mvir=1.23456789e11)]
            )
        ]
        path = fixtures.write_ctrees_file(self.dir / "cast.dat", trees)
        records, _, _ = parse_file(path)
        self.assertEqual(records["Mvir"][0], f32("{:.5e}".format(1.23456789e11)))

    def test_header_only_file_parses_empty(self):
        path = self.dir / "empty.dat"
        path.write_text(fixtures.header_line() + "\n")
        records, result, prescan = parse_file(path)
        self.assertEqual(len(records), 0)
        self.assertEqual(prescan.n_rows, 0)
        self.assertTrue(result.complete)

    def test_tree_count_line_skipped_and_recorded(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        path = fixtures.write_ctrees_file(self.dir / "counted.dat", trees, include_tree_count=True)
        records, result, prescan = parse_file(path)
        self.assertEqual(prescan.declared_tree_count, len(trees))
        self.assertEqual(len(records), sum(len(t.halos) for t in trees))
        self.assertTrue(result.complete)

    def test_file_without_tree_count_line_still_parses(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        path = fixtures.write_ctrees_file(
            self.dir / "uncounted.dat", trees, include_tree_count=False
        )
        _, result, prescan = parse_file(path)
        self.assertIsNone(prescan.declared_tree_count)
        self.assertTrue(result.complete)

    def test_inline_hash_in_data_row_aborts(self):
        path = self._write_with_token("inlinehash.dat", "Rvir", "150.0#tail")
        with self.assertRaisesRegex(ConverterError, "inline '#'"):
            prescan_file(path)

    def test_tree_prefix_comment_is_not_a_marker(self):
        # '#treejunk 99' must read as an ordinary comment, not a marker
        trees = [
            fixtures.TreeSpec(
                root_id=10,
                halos=[
                    fixtures.HaloSpec(halo_id=10, snap=5, mvir=1e11),
                    fixtures.HaloSpec(halo_id=11, snap=4, mvir=9e10, desc_id=10),
                ],
            )
        ]
        path = fixtures.write_ctrees_file(self.dir / "prefix.dat", trees)
        lines = path.read_text().splitlines()
        lines.insert(-1, "#treejunk 99")  # between the two data rows
        path.write_text("\n".join(lines) + "\n")
        records, _, prescan = parse_file(path)
        self.assertEqual(prescan.tree_root_ids.tolist(), [10])
        np.testing.assert_array_equal(records["tree_root_id"], [10, 10])

    def test_marker_with_extra_tokens_aborts(self):
        path = self.dir / "badmarker2.dat"
        path.write_text(fixtures.header_line() + "\n#tree 1 2\n")
        with self.assertRaisesRegex(ConverterError, "malformed '#tree' marker"):
            prescan_file(path)

    def test_bare_marker_token_aborts(self):
        path = self.dir / "badmarker3.dat"
        path.write_text(fixtures.header_line() + "\n#tree\n")
        with self.assertRaisesRegex(ConverterError, "malformed '#tree' marker"):
            prescan_file(path)

    def test_second_bare_count_line_aborts(self):
        path = self.dir / "twocounts.dat"
        path.write_text(fixtures.header_line() + "\n2\n3\n#tree 1\n")
        with self.assertRaisesRegex(ConverterError, "before the first '#tree'"):
            prescan_file(path)

    def test_data_row_before_first_tree_marker_aborts(self):
        path = self.dir / "orphan.dat"
        golden = (DATA_DIR / "indexed_header.dat").read_text().splitlines()
        self.assertFalse(golden[4].startswith("#"))  # guard against fixture layout drift
        path.write_text("\n".join([golden[0], golden[4]]) + "\n")
        with self.assertRaisesRegex(ConverterError, "before the first '#tree'"):
            prescan_file(path)

    def test_zero_byte_file_aborts(self):
        path = self.dir / "zero.dat"
        path.write_bytes(b"")
        with self.assertRaisesRegex(ConverterError, "empty file"):
            prescan_file(path)

    def test_snapshot_outside_int32_aborts(self):
        for snap_token in ("2147483648", "-2147483649"):  # INT32_MAX+1, INT32_MIN-1
            path = self._write_with_token("bigsnap.dat", "Snap_num", snap_token)
            with self.assertRaisesRegex(ConverterError, "outside int32 range"):
                parse_file(path)

    def test_snapshot_at_int32_limits_parses(self):
        records, _, _ = parse_file(self._write_with_token("edgesnap.dat", "Snap_num", "2147483647"))
        self.assertEqual(records["snap"][-1], 2147483647)

    def test_missing_header_aborts(self):
        path = self.dir / "noheader.dat"
        path.write_text("1.0 1 0.9 -1 -1 -1 1e12 1 1 1 1 1 1 1 1 1 1 1 5\n")
        with self.assertRaisesRegex(ConverterError, "first line"):
            prescan_file(path)

    def test_malformed_tree_marker_aborts(self):
        path = self.dir / "badmarker.dat"
        path.write_text(fixtures.header_line() + "\n#tree not_an_id\n")
        with self.assertRaisesRegex(ConverterError, "#tree"):
            prescan_file(path)

    def test_precount_mismatch_detected(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        path = fixtures.write_ctrees_file(self.dir / "trees.dat", trees)
        doctored = prescan_file(path)
        doctored.n_rows += 1
        parser = CtreesFileParser(path, prescan=doctored)
        with self.assertRaisesRegex(ConverterError, "pre-count"):
            list(parser.chunks())

    def test_precount_overrun_detected(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        path = fixtures.write_ctrees_file(self.dir / "trees.dat", trees)
        doctored = prescan_file(path)
        doctored.n_rows -= 1
        parser = CtreesFileParser(path, prescan=doctored)
        with self.assertRaisesRegex(ConverterError, "pre-count"):
            list(parser.chunks())


def _one_tree(halos, root=1):
    return [fixtures.TreeSpec(root_id=root, halos=halos)]


def _plain_halos(n, snap=5, first_id=11):
    return [fixtures.HaloSpec(halo_id=first_id + k, snap=snap, mvir=1.0e11) for k in range(n)]


def _parse(path, schema, chunksize=1000, forest_of_tree=None):
    """Parse ``path`` in the schema's extended layout; every tree is its own
    forest unless ``forest_of_tree`` (root id -> forest id) says otherwise."""
    prescan = prescan_file(path)
    roots = prescan.tree_root_ids
    forests = np.array(
        [forest_of_tree.get(int(r), int(r)) if forest_of_tree else int(r) for r in roots],
        dtype=np.int64,
    )
    parser = CtreesFileParser(path, chunksize=chunksize, prescan=prescan, schema=schema)
    parser.unit_plan = plan_source_units(prescan, forests, 0)
    parts = list(parser.chunks())
    return np.concatenate(parts) if parts else np.zeros(0, dtype=parser.dtype)


class TestProfileSelectionEquivalence(unittest.TestCase):
    """The shipped default profile selects exactly the legacy columns."""

    def test_default_profile_resolves_to_the_legacy_indices(self):
        schema = build_schema(load_column_map(DEFAULT_ASCII_PROFILE))
        for dialect in ("indexed", "fields"):
            for spelling in ("Snap_num", "snap_idx"):
                names = parse_header_line(fixtures.header_line(dialect, spelling))
                legacy = resolve_columns(names)
                selected = resolve_selection(names, schema)
                legacy_indices = dict(legacy.indices)
                legacy_indices["snap"] = legacy_indices.pop(legacy.snapshot_column)
                self.assertEqual(selected.indices, legacy_indices)
                self.assertEqual(selected.snapshot_column, spelling.lower())
                self.assertEqual(selected.extras, ())

    def test_default_profile_parses_legacy_fields_bit_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fixtures.write_ctrees_file(
                Path(tmp) / "t.dat", fixtures.all_trees(fixtures.standard_forests())
            )
            legacy, _, _ = parse_file(path, chunksize=3)
            extended = _parse(path, build_schema(load_column_map(DEFAULT_ASCII_PROFILE)), 3)
        for name in RECORD_DTYPE.names:
            self.assertEqual(extended[name].tobytes(), legacy[name].tobytes(), name)
        self.assertEqual(
            extended.dtype.names,
            RECORD_DTYPE.names + ("src_file_ordinal", "src_unit_ordinal", "src_row_ordinal"),
        )


class TestProfileSelectionRejections(unittest.TestCase):
    def names(self, *extra_names):
        return parse_header_line(fixtures.header_line("indexed", "Snap_num", extra_names))

    def test_both_snapshot_spellings_are_ambiguous(self):
        names = self.names("snap_idx")
        with self.assertRaisesRegex(ConverterError, "ambiguous"):
            resolve_selection(names, ascii_schema())

    def test_duplicate_selected_role_column_aborts(self):
        with self.assertRaisesRegex(ConverterError, "duplicate required column"):
            resolve_selection(self.names("MVIR"), ascii_schema())

    def test_duplicate_unselected_column_is_ignored_as_in_legacy(self):
        # real headers repeat b_to_a / A[x] once suffix-stripped
        names = self.names("b_to_a(500c)", "b_to_a")
        self.assertEqual(resolve_selection(names, ascii_schema()).extras, ())

    def test_duplicate_selected_extra_column_is_ambiguous(self):
        schema = ascii_schema([extra("BtoA", ["b_to_a"], "float")])
        with self.assertRaisesRegex(ConverterError, "carries this column 2 times"):
            resolve_selection(self.names("b_to_a(500c)", "b_to_a"), schema)

    def test_missing_extra_column_aborts(self):
        schema = ascii_schema([extra("Missing", ["no_such_column"], "float")])
        with self.assertRaisesRegex(ConverterError, "does not carry field"):
            resolve_selection(self.names(), schema)

    def test_component_on_an_ascii_column_aborts(self):
        schema = ascii_schema([extra("RvirX", ["Rvir"], "float", component=0)])
        with self.assertRaisesRegex(ConverterError, "scalars"):
            resolve_selection(self.names(), schema)

    def test_integer_extra_from_a_floating_role_aborts(self):
        schema = ascii_schema([extra("MassInt", ["Mvir"], "long long")])
        with self.assertRaisesRegex(ConverterError, "floating required role 'mvir'"):
            resolve_selection(self.names(), schema)

    def test_floating_extra_from_an_integer_role_aborts(self):
        schema = ascii_schema([extra("IdFloat", ["id"], "double")])
        with self.assertRaisesRegex(ConverterError, "integer required role 'id'"):
            resolve_selection(self.names(), schema)

    def test_one_column_declared_integer_and_floating_aborts(self):
        schema = ascii_schema(
            [extra("NumProgI", ["num_prog"], "int"), extra("NumProgF", ["num_prog"], "float")]
        )
        with self.assertRaisesRegex(ConverterError, "different number class"):
            resolve_selection(self.names(), schema)

    def test_two_roles_on_one_column_abort(self):
        with self.assertRaisesRegex(ConverterError, "same source field|both resolve"):
            resolve_selection(self.names(), ascii_schema(vrms=["vmax"]))

    def test_role_alias_resolves_case_insensitively_and_suffix_stripped(self):
        schema = ascii_schema(mvir=["MVIR(10)"])
        layout = resolve_selection(self.names(), schema)
        self.assertEqual(layout.indices["mvir"], fixtures.COLUMNS.index("Mvir"))


#: literal integer tokens above 2**53 and at the int64 limits
BIG_INTEGERS = ["9007199254740993", "-9223372036854775808", "9223372036854775807", "+17"]


class TestExtraParsing(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, tokens, name="Custom"):
        """One tree whose halo k carries ``tokens[k]`` in column ``name``."""
        halos = _plain_halos(len(tokens))
        text = {h.halo_id: t for h, t in zip(halos, tokens)}
        return fixtures.write_ctrees_file(
            self.dir / "t.dat",
            _one_tree(halos),
            extra_columns=[(name, lambda h: text[h.halo_id])],
        )

    def test_all_declared_types_carry_their_literal_source_values(self):
        trees = fixtures.all_trees(fixtures.standard_forests())
        path = fixtures.write_ctrees_file(self.dir / "t.dat", trees)
        records = _parse(path, build_schema(load_column_map(ALL_TYPES_PROFILE)), chunksize=4)
        rows = {int(r["id"]): r for r in records}
        for tree in trees:
            for halo in tree.halos:
                row = rows[halo.halo_id]
                self.assertEqual(row["extra_NumProg"], halo.num_prog)
                self.assertEqual(row["extra_NumProg"].dtype, np.dtype("<i4"))
                self.assertEqual(row["extra_HaloID"], halo.halo_id)
                self.assertEqual(row["extra_TreeRoot"], tree.root_id)
                self.assertEqual(row["extra_CatalogRvir"].tobytes(), f32("150.0").tobytes())
                self.assertEqual(row["extra_RvirDouble"], np.float64("150.0"))
                raw_j = [f32("{:.5e}".format(v)) for v in (halo.jx, halo.jy, halo.jz)]
                self.assertEqual(row["extra_RawJ"].tobytes(), np.array(raw_j).tobytes())
                self.assertEqual(row["extra_Counters"].tolist(), [halo.num_prog, 0, halo.snap])

    def test_integers_above_2_53_stay_exact(self):
        path = self.write(BIG_INTEGERS)
        records = _parse(path, ascii_schema([extra("Big", ["Custom"], "long long")]))
        self.assertEqual(records["extra_Big"].tolist(), [int(t) for t in BIG_INTEGERS])
        self.assertEqual(int(records["extra_Big"][0]) - 1, 2**53)

    def test_fraction_in_an_integer_column_aborts_with_row_and_column(self):
        path = self.write(["5", "6", "1.5"])
        with self.assertRaisesRegex(
            ConverterError, r"column 'custom \[extra Big, long long\]'.*ordinal\(s\) \[2\].*'1\.5'"
        ):
            _parse(path, ascii_schema([extra("Big", ["Custom"], "long long")]))

    def test_exponent_underscore_and_na_tokens_abort(self):
        for bad in ("1e3", "1_000", "nan", "NA"):
            with self.subTest(token=bad):
                path = self.write(["5", bad])
                with self.assertRaisesRegex(ConverterError, r"not integer literals.*\[1\]"):
                    _parse(path, ascii_schema([extra("Big", ["Custom"], "long long")]))

    def test_int64_overflow_aborts_with_row(self):
        path = self.write(["1", "9223372036854775808"])
        with self.assertRaisesRegex(ConverterError, r"overflowing int64.*\[1\]"):
            _parse(path, ascii_schema([extra("Big", ["Custom"], "long long")]))

    def test_int32_range_is_enforced_for_int(self):
        path = self.write(["2147483647", "-2147483648", "2147483648"])
        with self.assertRaisesRegex(ConverterError, r"outside the int32 range.*\[2\]"):
            _parse(path, ascii_schema([extra("Small", ["Custom"], "int")]))
        path = self.write(["2147483647", "-2147483648"])
        records = _parse(path, ascii_schema([extra("Small", ["Custom"], "int")]))
        self.assertEqual(records["extra_Small"].tolist(), [2147483647, -2147483648])

    def test_non_finite_float_extra_aborts_with_row_and_column(self):
        for bad in ("inf", "-inf", "nan"):
            with self.subTest(token=bad):
                path = self.write(["1.0", bad])
                with self.assertRaisesRegex(
                    ConverterError, r"non-finite.*custom \[extra F, double\].*\[1\]"
                ):
                    _parse(path, ascii_schema([extra("F", ["Custom"], "double")]))

    def test_float32_overflow_aborts_for_float_but_not_double(self):
        path = self.write(["1.0", "1e39"])
        with self.assertRaisesRegex(ConverterError, r"float32-overflowing.*\[1\]"):
            _parse(path, ascii_schema([extra("F", ["Custom"], "float")]))
        records = _parse(path, ascii_schema([extra("F", ["Custom"], "double")]))
        self.assertEqual(records["extra_F"].tolist(), [1.0, 1e39])

    def test_signed_zero_and_underflow_follow_the_declared_cast(self):
        path = self.write(["-0.0", "1e-50", "0.1"])
        records = _parse(path, ascii_schema([extra("F", ["Custom"], "float")]))
        values = records["extra_F"]
        self.assertTrue(np.signbit(values[0]) and values[0] == 0.0)
        self.assertEqual(values[1].tobytes(), np.float64(1e-50).astype(np.float32).tobytes())
        self.assertEqual(values[2].tobytes(), f32("0.1").tobytes())

    def test_integer_literal_float_column_parses_as_declared(self):
        # micro-Uchuu prints Mvir_all as integer literals; declaration governs
        path = self.write(["311000000000000", "1"])
        records = _parse(path, ascii_schema([extra("MvirAll", ["Custom"], "double")]))
        self.assertEqual(records["extra_MvirAll"].tolist(), [3.11e14, 1.0])


class TestSourceUnits(unittest.TestCase):
    def test_plan_groups_interleaved_trees_by_forest_first_marker(self):
        # trees in file order: roots 5, 6, 7, 8 with sizes 2, 3, 1, 4;
        # forests A=100 (roots 5, 7) and B=200 (roots 6, 8)
        with tempfile.TemporaryDirectory() as tmp:
            trees = [
                fixtures.TreeSpec(root_id=5, halos=_plain_halos(2, first_id=50)),
                fixtures.TreeSpec(root_id=6, halos=_plain_halos(3, first_id=60)),
                fixtures.TreeSpec(root_id=7, halos=_plain_halos(1, first_id=70)),
                fixtures.TreeSpec(root_id=8, halos=_plain_halos(4, first_id=80)),
            ]
            path = fixtures.write_ctrees_file(Path(tmp) / "t.dat", trees)
            prescan = prescan_file(path)
            plan = plan_source_units(prescan, np.array([100, 200, 100, 200]), 3)
            self.assertEqual(plan.unit_of_tree.tolist(), [0, 1, 0, 1])
            self.assertEqual(plan.unit_row_base.tolist(), [0, 0, 2, 3])
            self.assertEqual(plan.unit_forest_ids.tolist(), [100, 200])
            self.assertEqual(plan.unit_counts.tolist(), [3, 7])
            self.assertEqual(plan.table().tolist(), [[100, 3], [200, 7]])
            forest_of = {5: 100, 6: 200, 7: 100, 8: 200}
            for chunksize in (1, 2, 3, 10):
                prescan = prescan_file(path)
                parser = CtreesFileParser(
                    path, chunksize=chunksize, prescan=prescan, schema=ascii_schema()
                )
                parser.unit_plan = plan_source_units(
                    prescan, np.array([forest_of[int(r)] for r in prescan.tree_root_ids]), 3
                )
                records = np.concatenate(list(parser.chunks()))
                got = {
                    int(r["id"]): (
                        int(r["src_file_ordinal"]),
                        int(r["src_unit_ordinal"]),
                        int(r["src_row_ordinal"]),
                    )
                    for r in records
                }
                expected = {50: (3, 0, 0), 51: (3, 0, 1), 70: (3, 0, 2)}
                expected.update({60 + k: (3, 1, k) for k in range(3)})
                expected.update({80 + k: (3, 1, 3 + k) for k in range(4)})
                self.assertEqual(got, expected, "chunksize {}".format(chunksize))

    def test_extended_parse_without_a_plan_aborts(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fixtures.write_ctrees_file(Path(tmp) / "t.dat", _one_tree(_plain_halos(2)))
            parser = CtreesFileParser(path, schema=ascii_schema())
            with self.assertRaisesRegex(ConverterError, "source-unit plan"):
                list(parser.chunks())


class TestScratchLayout(unittest.TestCase):
    def test_legacy_layout_is_the_frozen_record(self):
        self.assertIs(LEGACY_LAYOUT.dtype, RECORD_DTYPE)
        self.assertEqual(LEGACY_LAYOUT.dtype_tag, DTYPE_TAG)
        self.assertFalse(LEGACY_LAYOUT.is_extended)

    def test_extended_layout_width_and_tag(self):
        schema = build_schema(load_column_map(ALL_TYPES_PROFILE))
        layout = ScratchLayout.from_schema(schema)
        # 108 frozen + 3 x 8 coordinates + 4+8+8+4+8+12+12 extras (sorted)
        self.assertEqual(layout.dtype.itemsize, 108 + 24 + 56)
        self.assertEqual(
            [n for n in layout.dtype.names if n.startswith("extra_")],
            ["extra_" + n for n in sorted(e.name for e in schema.extra_fields)],
        )
        self.assertTrue(layout.dtype_tag.startswith("ctrees-scratch-v2/itemsize=188/"))
        self.assertIn("extra_RawJ:<f4[3]", layout.dtype_tag)
        self.assertNotEqual(layout.dtype_tag, DTYPE_TAG)
        self.assertEqual(ScratchLayout.from_record(layout.to_record(), "t"), layout)

    def test_zero_extra_layout_still_carries_source_keys(self):
        layout = ScratchLayout.from_schema(ascii_schema())
        self.assertEqual(layout.dtype.itemsize, 132)
        self.assertTrue(layout.is_extended)

    def test_wide_schema(self):
        extras = [extra("Wide{:02d}".format(k), ["Rvir"], "double") for k in range(40)]
        layout = ScratchLayout.from_schema(ascii_schema(extras))
        self.assertEqual(layout.dtype.itemsize, 132 + 40 * 8)

    def test_same_width_schemas_differ_in_digest(self):
        a = ScratchLayout.from_schema(ascii_schema([extra("E", ["Rvir"], "float")]))
        changed = extra("E", ["Rvir"], "float")
        changed["units"] = "Mpc/h"
        b = ScratchLayout.from_schema(ascii_schema([changed]))
        self.assertEqual(a.dtype_tag, b.dtype_tag)
        self.assertNotEqual(a, b)
        c = ScratchLayout.from_schema(ascii_schema([extra("E", ["num_prog"], "int")]))
        self.assertEqual(a.dtype.itemsize, c.dtype.itemsize)
        self.assertNotEqual(a.dtype_tag, c.dtype_tag)

    def test_tampered_layout_record_is_refused(self):
        record = ScratchLayout.from_schema(
            ascii_schema([extra("E", ["Rvir"], "float")])
        ).to_record()
        for key, value in (
            ("extras", [["E", "double"]]),
            ("extras", [["Z", "float"], ["E", "float"]]),
            ("schema_digest", "not-a-digest"),
            ("version", 2),
        ):
            with self.subTest(key=key):
                tampered = dict(record, **{key: value})
                with self.assertRaises(ConverterError):
                    ScratchLayout.from_record(tampered, "t")
        with self.assertRaisesRegex(ConverterError, "malformed"):
            ScratchLayout.from_record({"version": 1}, "t")


if __name__ == "__main__":
    unittest.main()
