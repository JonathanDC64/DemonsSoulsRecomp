"""Tests for tools/release/third_party_sources.py, with no network."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "release"))
import third_party_sources as tps  # noqa: E402


class CheckTest(unittest.TestCase):
    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = tps.main(list(args))
        return rc, out.getvalue(), err.getvalue()

    def test_check_asks_the_release_for_each_archive(self):
        with mock.patch.object(tps, "fetch_any") as fetch:
            rc, out, _ = self.run_main("--check")
        self.assertEqual(rc, 0)
        urls = [c.args[0] for c in fetch.call_args_list]
        self.assertEqual(len(urls), len(tps.SOURCES))
        for [url] in urls:
            self.assertTrue(url.startswith(tps.HOME + "/"), url)
        self.assertEqual(out.count("ok "), len(tps.SOURCES))

    def test_check_fails_on_a_missing_archive_and_says_what_to_do(self):
        with mock.patch.object(tps, "fetch_any", side_effect=OSError("404")):
            rc, _, err = self.run_main("--check")
        self.assertEqual(rc, 1)
        self.assertIn("gh release upload", err)

    def test_check_fails_on_the_wrong_bytes(self):
        with mock.patch.object(tps, "fetch_any", side_effect=ValueError("SHA-256")):
            self.assertEqual(self.run_main("--check")[0], 1)

    def test_one_of_the_two_is_needed(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
            tps.main([])
        self.assertEqual(e.exception.code, 2)

    def test_out_takes_them_from_their_own_projects(self):
        with mock.patch.object(tps, "fetch_any") as fetch, mock.patch.object(tps.os, "makedirs"):
            rc, _, _ = self.run_main("--out", "somewhere")
        self.assertEqual(rc, 0)
        self.assertEqual(fetch.call_count, len(tps.SOURCES))
        for call in fetch.call_args_list:
            self.assertFalse(any(u.startswith(tps.HOME) for u in call.args[0]))


if __name__ == "__main__":
    unittest.main()
