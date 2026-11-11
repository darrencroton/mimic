"""The design: sizes, row order, stratification, random streams, round trip and determinism,
and the checks on a design made elsewhere."""

import copy
import hashlib
import itertools
import math
import os
import subprocess
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from common import BENCH_DIR, SHIPPED_SPEC, raw_spec, shipped_spec, tmp_path  # noqa: E402
from mimic_bench.cli import main  # noqa: E402
from mimic_bench.design import (  # noqa: E402
    DesignError,
    build_design,
    corners,
    design_columns,
    latin_hypercube,
    parse_design,
    set_rng,
    write_design,
)
from mimic_bench.spec import SET_NAMES, parse_spec  # noqa: E402

ROWS = None
"""The shipped spec's design rows, built once per module by :func:`setUpModule`."""


def setUpModule():
    """Build the shipped spec's design rows once."""
    global ROWS
    ROWS = build_design(shipped_spec())


def _values(rows, set_name):
    """The values of one set as an array of shape (rows, dimensions)."""
    return np.array([r.values for r in rows if r.set_name == set_name])


def _strata(parameter, values):
    """The index of the equal-width interval, in the parameter's own scale, of each value."""
    n = len(values)
    if parameter.scale == "log":
        position = (np.log10(values) - math.log10(parameter.low)) / (
            math.log10(parameter.high) - math.log10(parameter.low)
        )
    else:
        position = (values - parameter.low) / (parameter.high - parameter.low)
    return np.floor(position * n).astype(int)


class TestDesign(unittest.TestCase):
    """The design table built from the shipped spec and from variations of it."""

    def test_sizes_columns_and_identifiers(self):
        """One row per evaluation: 133 pilot, 400 train and 200 holdout, uniquely identified."""
        spec, rows = shipped_spec(), ROWS
        counts = {name: sum(r.set_name == name for r in rows) for name in SET_NAMES}
        self.assertEqual(counts, {"pilot": 133, "train": 400, "holdout": 200})
        self.assertEqual(len(rows), 733)
        self.assertEqual(len({r.identifier for r in rows}), 733)
        self.assertEqual(
            design_columns(spec),
            [
                "id",
                "set",
                "SfrEfficiency",
                "FeedbackReheatingEpsilon",
                "FeedbackEjectionEfficiency",
                "ReIncorporationFactor",
                "RadioModeEfficiency",
                "InertControl",
            ],
        )
        self.assertEqual(rows[0].identifier, "pilot-0000")
        self.assertEqual(rows[-1].identifier, "holdout-0199")
        self.assertTrue(all(len(r.values) == 6 for r in rows))

    def test_pilot_starts_with_the_defaults_then_the_corners(self):
        """The first pilot row holds every default and the next 32 are the corners, in order."""
        spec = shipped_spec()
        pilot = _values(ROWS, "pilot")
        defaults = [p.default for p in spec.all_parameters]
        self.assertEqual(pilot[0].tolist(), defaults)
        corner_rows = pilot[1:33]
        self.assertEqual(len({tuple(r) for r in corner_rows}), 32)
        expected = [
            [*corner, spec.inert_control.default]
            for corner in itertools.product(*[(p.low, p.high) for p in spec.parameters])
        ]
        self.assertEqual(corner_rows.tolist(), expected)
        self.assertEqual(corner_rows[0].tolist()[:5], [p.low for p in spec.parameters])
        self.assertEqual(corner_rows[-1].tolist()[:5], [p.high for p in spec.parameters])
        self.assertTrue((corner_rows[:, 5] == spec.inert_control.default).all())
        self.assertEqual(corners(spec).shape, (32, 6))

    def test_latin_hypercube_is_stratified_in_each_scale_and_strictly_inside(self):
        """Each of the n equal-probability intervals holds exactly one point, strictly in range."""
        spec = shipped_spec()
        for set_name, start in [("pilot", 33), ("train", 0), ("holdout", 0)]:
            with self.subTest(set_name=set_name, start=start):
                points = _values(ROWS, set_name)[start:]
                n = len(points)
                self.assertEqual(n, spec.design.sets[set_name].latin_hypercube)
                for j, parameter in enumerate(spec.all_parameters):
                    column = points[:, j]
                    self.assertTrue((column > parameter.low).all())
                    self.assertTrue((column < parameter.high).all())
                    self.assertEqual(
                        sorted(_strata(parameter, column).tolist()),
                        list(range(n)),
                        parameter.name,
                    )

    def test_linear_scale_parameter_spanning_zero_is_stratified_linearly(self):
        """A linear parameter over a range spanning zero is sampled and stratified in the value."""
        raw = raw_spec()
        raw["parameters"][0].update(
            {"name": "Offset", "low": -1.0, "high": 2.0, "default": 0.5, "scale": "linear"}
        )
        raw["design"]["sets"]["train"]["latin_hypercube"] = 50
        spec = parse_spec(raw)
        train = _values(build_design(spec), "train")
        offset = train[:, 0]
        self.assertLess(offset.min(), 0)
        self.assertGreater(offset.max(), 0)
        self.assertTrue((offset > -1.0).all())
        self.assertTrue((offset < 2.0).all())
        width = 3.0 / 50
        self.assertEqual(
            sorted(np.floor((offset + 1.0) / width).astype(int).tolist()), list(range(50))
        )
        for j, parameter in enumerate(spec.all_parameters):
            self.assertEqual(sorted(_strata(parameter, train[:, j]).tolist()), list(range(50)))

    def test_the_three_sets_come_from_distinct_streams_of_one_seed(self):
        """Each set has its own stream, fixed by the seed and the set's name alone."""
        spec, raw = shipped_spec(), raw_spec()
        seed = spec.design.seed
        firsts = [set_rng(seed, name).random() for name in SET_NAMES]
        self.assertEqual(len(set(firsts)), 3)
        self.assertEqual(set_rng(seed, "train").random(), set_rng(seed, "train").random())
        self.assertNotEqual(set_rng(seed + 1, "train").random(), set_rng(seed, "train").random())

        base = build_design(spec)
        raw["design"]["sets"]["train"]["latin_hypercube"] = 123
        resized = build_design(parse_spec(raw))
        for name in ("pilot", "holdout"):
            self.assertEqual(_values(base, name).tolist(), _values(resized, name).tolist())
        self.assertEqual(len(_values(resized, "train")), 123)

        raw["design"]["seed"] = seed + 1
        reseeded = build_design(parse_spec(raw))
        for name in ("pilot", "train", "holdout"):
            self.assertNotEqual(
                _values(base, name)[-5:].tolist(), _values(reseeded, name)[-5:].tolist()
            )

    def test_no_two_lhs_points_repeat_across_sets(self):
        """The sets' Latin-hypercube points are distinct (no stream was reused)."""
        lhs = [
            r.values for r in ROWS if not (r.set_name == "pilot" and int(r.identifier[-4:]) < 33)
        ]
        self.assertEqual(len(set(lhs)), len(lhs))
        self.assertEqual(len(lhs), 700)

    def test_values_round_trip_exactly_and_the_file_is_reproducible(self):
        """Reading the file back gives the same floats, and two writes give identical bytes."""
        spec, directory = shipped_spec(), tmp_path(self)
        first, second = directory / "a" / "design.csv", directory / "b.csv"
        write_design(ROWS, spec, first)
        write_design(build_design(spec), spec, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertNotIn(b"\r", first.read_bytes())
        self.assertEqual(parse_design(first.read_bytes(), first, spec), ROWS)
        lines = first.read_text().splitlines()
        self.assertEqual(len(lines), 734)
        self.assertEqual(lines[0], ",".join(design_columns(spec)))

    def test_two_command_line_invocations_write_byte_identical_files(self):
        """Two ``design`` invocations, in separate processes, write the same bytes."""
        directory = tmp_path(self)
        outputs = [directory / "one.csv", directory / "nested" / "two.csv"]
        for out in outputs:
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "mimic_bench",
                    "design",
                    "--spec",
                    str(SHIPPED_SPEC),
                    "--out",
                    str(out),
                ],
                cwd=BENCH_DIR,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(outputs[0].read_bytes(), outputs[1].read_bytes())
        self.assertEqual(len(outputs[0].read_text().splitlines()), 734)

    def test_design_stage_replaces_an_existing_file(self):
        """Running the stage again over its own output succeeds and leaves the same bytes."""
        out = tmp_path(self) / "design.csv"
        argv = ["design", "--spec", str(SHIPPED_SPEC), "--out", str(out)]
        self.assertEqual(main(argv), 0)
        first = out.read_bytes()
        self.assertEqual(main(argv), 0)
        self.assertEqual(out.read_bytes(), first)

    def test_defaults_and_corners_follow_the_spec_for_any_set(self):
        """Any set may ask for default and corner rows, and corners number 2 ** parameters."""
        raw = raw_spec()
        raw["parameters"] = raw["parameters"][:2]
        raw["design"]["sets"]["train"].update(
            {"include_defaults": True, "include_corners": True, "latin_hypercube": 7}
        )
        raw["design"]["sets"]["holdout"]["latin_hypercube"] = 3
        raw["design"]["sets"]["pilot"] = {"latin_hypercube": 4}
        raw["outputs"]["selection_rule"]["min_runs"] = 3
        spec = parse_spec(raw)
        rows = build_design(spec)
        sizes = {name: sum(r.set_name == name for r in rows) for name in SET_NAMES}
        self.assertEqual(sizes, {"pilot": 4, "train": 1 + 4 + 7, "holdout": 3})
        train = _values(rows, "train")
        self.assertEqual(train[0].tolist(), [p.default for p in spec.all_parameters])
        self.assertEqual(len({tuple(r) for r in train[1:5]}), 4)

    def test_latin_hypercube_keeps_values_off_the_bounds(self):
        """Even a draw that rounds onto a bound is moved strictly inside the range."""
        spec = shipped_spec()

        class EdgeRng:
            """A stand-in generator whose draws sit on the unit interval's edges."""

            def permutation(self, n):
                return np.arange(n)

            def random(self, n):
                return np.where(np.arange(n) % 2 == 0, 0.0, np.nextafter(1.0, 0.0))

        points = latin_hypercube(2, spec.all_parameters, EdgeRng())
        for j, parameter in enumerate(spec.all_parameters):
            self.assertTrue((points[:, j] > parameter.low).all())
            self.assertTrue((points[:, j] < parameter.high).all())

    def test_design_is_independent_of_dict_order(self):
        """The sets' streams follow the set names, not the order they are written in the spec."""
        raw = raw_spec()
        base = build_design(shipped_spec())
        sets = raw["design"]["sets"]
        raw["design"]["sets"] = {k: sets[k] for k in reversed(list(sets))}
        reordered = build_design(parse_spec(copy.deepcopy(raw)))
        self.assertEqual(reordered, base)


UNIT_STREAM_SHA256 = "3783fe48a9efab6975efb2f6d43531a4abc600ee531baecec395132040d77af0"
"""The design file of the shipped spec's seed and set sizes with every dimension linear on
``[0, 1]`` (see :meth:`TestDesignPin.test_the_latin_hypercube_stream_is_pinned`)."""


class TestDesignPin(unittest.TestCase):
    """The random stream behind every design is pinned, so a numpy upgrade cannot move it."""

    def test_the_latin_hypercube_stream_is_pinned(self):
        """``design`` of the shipped seed and set sizes, every dimension linear on [0, 1].

        With those ranges the design's values are its unit-cube draws (``(permutation +
        random) / n``, then ``0 + u * 1``), all exact in IEEE arithmetic, so the file's SHA-256
        depends only on numpy's ``SeedSequence``, ``PCG64``, ``permutation`` and ``random``: the
        pin fails when a numpy change moves the stream every design, and so every capture's rows,
        is drawn from. The shipped spec's own file also passes each draw through the platform's
        ``log10`` and ``pow``, whose last bit may differ between C libraries, so it is not hashed
        here.
        """
        raw = raw_spec()
        for parameter in [*raw["parameters"], raw["inert_control"]]:
            parameter.update(low=0.0, high=1.0, default=0.5, scale="linear")
        spec = parse_spec(raw)
        path = tmp_path(self) / "design.csv"
        write_design(build_design(spec), spec, path)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), UNIT_STREAM_SHA256)


class TestParseDesign(unittest.TestCase):
    """A design made elsewhere is held to the table ``design`` writes."""

    def setUp(self):
        """A valid two-row design with free-form set labels, as CSV text."""
        spec = shipped_spec()
        self.spec = spec
        self.header = ",".join(design_columns(spec))
        defaults = ",".join(repr(p.default) for p in spec.all_parameters)
        self.rows = [f"sobol-0,first wave,{defaults}", f"sobol-1,second,{defaults}"]

    def _parse(self, header=None, rows=None):
        text = "\n".join([header or self.header, *(self.rows if rows is None else rows)]) + "\n"
        return parse_design(text.encode(), SHIPPED_SPEC.parent / "external.csv", self.spec)

    def test_a_valid_design_with_free_labels_is_read(self):
        rows = self._parse()
        self.assertEqual(
            [(r.identifier, r.set_name) for r in rows],
            [
                ("sobol-0", "first wave"),
                ("sobol-1", "second"),
            ],
        )
        self.assertEqual(rows[0].values, tuple(p.default for p in self.spec.all_parameters))

    def test_each_defect_is_rejected_naming_it(self):
        defaults = [repr(p.default) for p in self.spec.all_parameters]
        name = self.spec.parameters[0].name
        low = self.spec.parameters[0].low

        def row(identifier="x", label="a", first=defaults[0]):
            return ",".join([identifier, label, first, *defaults[1:]])

        cases = {
            "header": (self.header.replace(name, "Other"), None, "header"),
            "missing column": (self.header.rsplit(",", 1)[0], None, "header"),
            "duplicate id": (None, [row("a"), row("a")], "more than one row"),
            "empty id": (None, [row("")], "empty id"),
            "path in id": (None, [row("../a")], "must match"),
            "empty label": (None, [row(label=" ")], "empty set label"),
            "below range": (None, [row(first=repr(low / 2))], name),
            "not finite": (None, [row(first="nan")], name),
            "not a number": (None, [row(first="big")], name),
            "blank line": (None, [row("a"), "", row("b")], "blank line"),
        }
        for case, (header, rows, message) in cases.items():
            with self.subTest(case=case):
                with self.assertRaisesRegex(DesignError, message):
                    self._parse(header, rows)


if __name__ == "__main__":
    unittest.main()
