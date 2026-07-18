"""Slice 2 unit tests: frozen dtype, header dialects, #tree attribution,
independent pre-count, malformed-input aborts."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fixtures  # noqa: E402
from ctrees_parser import (  # noqa: E402
    DTYPE_TAG,
    RECORD_DTYPE,
    ConverterError,
    CtreesFileParser,
    parse_file,
    parse_header_line,
    prescan_file,
    resolve_columns,
)

DATA_DIR = Path(__file__).parent / "data"


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


if __name__ == "__main__":
    unittest.main()
