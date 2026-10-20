"""The L-Halo record derived from a package's ``halo_properties.yaml`` (vertical/record.py).

Oracles: the compiled ``struct RawHalo`` layouts typed here literally -- the Shin-Uchuu record,
88 bytes with ``MostBoundID`` at 72, ``SnapNum`` at 80 and four bytes of trailing padding, and
the mini-Millennium record, 104 bytes with no padding -- and the C alignment rules for every type
the property generator knows; the generator's own type table for the set of accepted types.
"""

import json
import sys
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
sys.path.append(str(REPO_ROOT / "scripts"))

from errors import ConverterError  # noqa: E402
from vertical.record import (  # noqa: E402
    RECORD_TYPES,
    record_digest,
    record_dtype,
    record_layout,
)

SHIN_UCHUU = REPO_ROOT / "simulations" / "shin-uchuu-ascii" / "halo_properties.yaml"
MINI_MILLENNIUM = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"


def offsets(dtype):
    return {name: dtype.fields[name][1] for name in dtype.names}


class RecordLayoutTests(unittest.TestCase):
    def test_shin_uchuu_record_is_88_bytes_with_a_trailing_pad(self):
        dtype = record_dtype(SHIN_UCHUU)
        self.assertEqual(dtype.itemsize, 88)
        self.assertEqual(
            offsets(dtype),
            {
                "Descendant": 0,
                "FirstProgenitor": 4,
                "NextProgenitor": 8,
                "FirstHaloInFOFgroup": 12,
                "NextHaloInFOFgroup": 16,
                "Len": 20,
                "M_Crit200": 24,
                "Pos": 28,
                "Vel": 40,
                "Spin": 52,
                "VelDisp": 64,
                "Vmax": 68,
                "MostBoundID": 72,
                "SnapNum": 80,
            },
        )
        self.assertEqual(dtype.descr[-1], ("", "|V4"))
        self.assertEqual(np.zeros(2, dtype=dtype).strides, (88,))

    def test_mini_millennium_record_is_104_bytes_without_padding(self):
        dtype = record_dtype(MINI_MILLENNIUM)
        self.assertEqual(dtype.itemsize, 104)
        self.assertEqual(sum(dtype.fields[name][0].itemsize for name in dtype.names), 104)
        self.assertEqual(offsets(dtype)["MostBoundID"], 80)
        self.assertEqual(offsets(dtype)["SubHalfMass"], 100)

    def test_every_generator_type_takes_its_c_size_and_alignment(self):
        import generate_properties

        self.assertEqual(set(RECORD_TYPES), set(generate_properties.TYPE_MAP))
        entries = [
            {"name": "a", "type": "int"},
            {"name": "b", "type": "double"},
            {"name": "c", "type": "vec3_int"},
            {"name": "d", "type": "long long"},
            {"name": "e", "type": "float"},
            {"name": "f", "type": "vec3_float"},
        ]
        dtype = record_dtype(entries)
        self.assertEqual(offsets(dtype), {"a": 0, "b": 8, "c": 16, "d": 32, "e": 40, "f": 44})
        self.assertEqual(dtype.itemsize, 56)
        self.assertEqual(dtype["c"].shape, (3,))
        self.assertEqual(dtype["f"].base, np.dtype("<f4"))

    def test_a_parsed_list_derives_the_same_record_as_its_file(self):
        import yaml

        entries = yaml.safe_load(SHIN_UCHUU.read_text())["halo_properties"]
        self.assertEqual(record_dtype(entries), record_dtype(SHIN_UCHUU))

    def test_an_unknown_type_is_refused_naming_the_entry(self):
        entries = [{"name": "Len", "type": "int"}, {"name": "Mass", "type": "unsigned"}]
        with self.assertRaisesRegex(ConverterError, r"'Mass' has type 'unsigned'"):
            record_dtype(entries)
        with self.assertRaisesRegex(ConverterError, r"'Pos' has type \['float', 3\]"):
            record_dtype([{"name": "Pos", "type": ["float", 3]}])
        with self.assertRaisesRegex(ConverterError, r"'Len' is declared twice"):
            record_dtype([{"name": "Len", "type": "int"}] * 2)
        with self.assertRaisesRegex(ConverterError, "entry 0 has no name"):
            record_dtype([{"type": "int"}])
        with self.assertRaisesRegex(ConverterError, "no halo_properties list"):
            record_dtype(REPO_ROOT / "simulations" / "shin-uchuu-ascii" / "simulation_info.yaml")


class RecordDigestTests(unittest.TestCase):
    def test_the_layout_is_json_and_its_digest_is_stable(self):
        layout = record_layout(record_dtype(SHIN_UCHUU))
        self.assertEqual(json.loads(json.dumps(layout)), layout)
        self.assertEqual(layout["itemsize"], 88)
        self.assertEqual(
            layout["fields"][12],
            {"name": "MostBoundID", "offset": 72, "size": 8, "dtype": "<i8", "shape": []},
        )
        self.assertEqual(
            layout["fields"][7],
            {"name": "Pos", "offset": 28, "size": 12, "dtype": "<f4", "shape": [3]},
        )
        self.assertEqual(
            record_digest(layout),
            "ab04dcbae52ffd11b1e773fe3c9aafecc22371f4583eeadf2353930e5bdf5e64",
        )

    def test_the_digest_changes_with_a_type_or_an_order_but_not_a_description(self):
        base = [{"name": "a", "type": "int"}, {"name": "b", "type": "float"}]
        digest = record_digest(record_layout(record_dtype(base)))
        retyped = [{"name": "a", "type": "int"}, {"name": "b", "type": "int"}]
        reordered = list(reversed(base))
        described = [dict(entry, description="text") for entry in base]
        self.assertNotEqual(record_digest(record_layout(record_dtype(retyped))), digest)
        self.assertNotEqual(record_digest(record_layout(record_dtype(reordered))), digest)
        self.assertEqual(record_digest(record_layout(record_dtype(described))), digest)


if __name__ == "__main__":
    unittest.main()
