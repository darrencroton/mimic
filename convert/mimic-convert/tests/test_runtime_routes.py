"""The converter's evidenced-route text comes from one constant, runtime_routes.ROUTES."""

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import runtime_routes  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]


class RuntimeRoutesTests(unittest.TestCase):
    def test_routes_are_unique_and_safe_to_join(self):
        keys = [(route.simulation, route.source_format) for route in runtime_routes.ROUTES]
        self.assertEqual(len(set(keys)), len(keys), "a route is listed twice")
        for route in runtime_routes.ROUTES:
            with self.subTest(route=route.simulation, source_format=route.source_format):
                self.assertTrue(route.models)
                self.assertNotIn(";", route.coverage, "the renderers join routes with '; '")

    def test_every_renderer_names_the_routes_and_the_standing_text(self):
        self.assertIn("v3-runtime-support", runtime_routes.SPEC_ANCHOR)
        self.assertTrue((REPO_ROOT / runtime_routes.SPEC_ANCHOR.split("#")[0]).is_file())
        notice = runtime_routes.runtime_notice()
        report = " ".join(runtime_routes.standing_limitations())
        for text in (notice, report):
            for route in runtime_routes.ROUTES:
                self.assertIn(route.source_format, text)
                self.assertIn("{} {}, {}".format(*route[:3]), text)
                self.assertIn(" and ".join(route.models), text)
            self.assertIn(runtime_routes.SPEC_ANCHOR, text)
            self.assertIn(runtime_routes.FULL_UCHUU_NOT_CLAIMED, text)
        self.assertIn("a conversion is not a validated route", notice)
        description = runtime_routes.cli_description()
        for simulation in {r.simulation for r in runtime_routes.ROUTES}:
            self.assertIn(simulation, description)
        self.assertIn("full Uchuu is not claimed", description)


if __name__ == "__main__":
    unittest.main()
