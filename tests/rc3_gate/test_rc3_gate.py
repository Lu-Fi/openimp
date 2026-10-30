#!/usr/bin/env python3
"""Unit test of the RC3 gate rule (tools/release_gates.evaluate_rc3) with a fake docs/status/issues.json."""
import json
import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release_gates as rg  # noqa: E402
import status_data as sd  # noqa: E402

REL = sd.load_release()


def issue(n, labels=(), milestone=None, state="open"):
    return {"number": n, "title": "t%d" % n, "state": state, "url": "u", "labels": list(labels), "milestone": milestone}


def verdict(issues, cells_ok=True):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "issues.json")
        json.dump({"issues": issues}, open(p, "w"))
        return rg.evaluate_rc3(cells_ok, sd.load_issues(p), REL)


class Rc3Gate(unittest.TestCase):
    def test_green(self):
        v = verdict([issue(1, ["severity:medium"]), issue(2, ["severity:low"], "Release 1"), issue(3)])
        self.assertTrue(v["ok"])

    def test_closed_issues_do_not_block(self):
        v = verdict([issue(1, ["severity:critical"], "RC3", "closed")])
        self.assertTrue(v["ok"])

    def test_red_by_critical(self):
        v = verdict([issue(1, ["severity:critical"])])
        self.assertFalse(v["ok"])
        self.assertEqual([i["number"] for i in v["blockers"]["critical"]], [1])
        self.assertFalse(v["severity"])
        self.assertTrue(v["milestone"] and v["cells"])

    def test_red_by_high(self):
        v = verdict([issue(2, ["severity:high", "soc:T40"])])
        self.assertFalse(v["ok"])
        self.assertEqual([i["number"] for i in v["blockers"]["high"]], [2])

    def test_red_by_milestone(self):
        v = verdict([issue(3, ["severity:low"], "RC3")])
        self.assertFalse(v["ok"])
        self.assertEqual([i["number"] for i in v["blockers"]["milestone"]], [3])
        self.assertTrue(v["severity"] and v["cells"])

    def test_red_by_cells(self):
        v = verdict([], cells_ok=False)
        self.assertFalse(v["ok"])
        self.assertFalse(v["cells"])
        self.assertTrue(v["severity"] and v["milestone"])

    def test_rules_in_yaml(self):
        self.assertEqual(rg.rule(REL, "rc3", "block_milestone"), "RC3")
        self.assertEqual(rg.rule(REL, "rc3", "block_severity"), ["severity:critical", "severity:high"])


if __name__ == "__main__":
    unittest.main()
