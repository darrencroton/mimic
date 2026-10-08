"""The converter's evidenced-route text comes from one constant, runtime_routes.ROUTES."""

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import runtime_routes  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]


class RuntimeRoutesTests(unittest.TestCase):
    def test_constant_holds_the_six_gated_routes_with_their_coverage(self):
        rows = {(r.simulation, r.source_format): r for r in runtime_routes.ROUTES}
        self.assertEqual(len(runtime_routes.ROUTES), 6)
        self.assertEqual(len(rows), 6)
        for key in (
            ("mini-Millennium", "lhalo_binary"),
            ("micro-Uchuu", "lhalo_binary"),
            ("micro-Uchuu", "consistent_trees_hdf5"),
            ("Millennium", "lhalo_binary"),
            ("mini-Uchuu", "lhalo_binary"),
            ("micro-Uchuu", "consistent_trees_ascii"),
        ):
            self.assertEqual(rows[key].coverage, "complete")
        sage16_keys = {
            ("mini-Millennium", "lhalo_binary"),
            ("micro-Uchuu", "consistent_trees_ascii"),
        }
        for key in sage16_keys:
            self.assertEqual(rows[key].models, ("halos-only", "sage16"))
        others = [r for key, r in rows.items() if key not in sage16_keys]
        self.assertTrue(all(r.models == ("halos-only",) for r in others))

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
