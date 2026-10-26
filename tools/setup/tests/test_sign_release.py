"""Tests for tools/release/sign_release.py, with gh, git and ssh-keygen stood in for (docs/105 section 7).

  cd tools/setup && python -m unittest discover -s tests
"""
import contextlib
import hashlib
import io
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "release"))
from des_setup import signing  # noqa: E402
import sign_release  # noqa: E402

SEED, OTHER = bytes(range(32)), bytes(range(1, 33))
TAG = "v0.2.0"


class Fake:
    """The draft release and the tools: gh's view, download, attestation and upload; git's tags; ssh-keygen."""

    def __init__(self, files, draft=True, signer=SEED, attested=True):
        self.files, self.draft, self.signer, self.attested = files, draft, signer, attested
        self.uploaded = None
        self.sums = "".join(f"{hashlib.sha256(b).hexdigest()}  {n}\n" for n, b in sorted(files.items())).encode()

    def run(self, args, **kw):
        if args[:3] == ["gh", "release", "view"]:
            names = sorted(self.files) + ["SHA256SUMS"]
            return json.dumps({"isDraft": self.draft, "assets": [{"name": n} for n in names]})
        if args[:3] == ["gh", "release", "download"]:
            d = Path(args[args.index("--dir") + 1])
            for i, a in enumerate(args):
                if a == "--pattern":
                    n = args[i + 1]
                    (d / n).write_bytes(self.sums if n == "SHA256SUMS" else self.files[n])
            return ""
        if args[:3] == ["gh", "attestation", "verify"]:
            if not self.attested:
                raise sign_release.Error("gh attestation verify: no attestation")
            return ""
        if args[:3] == ["gh", "release", "upload"]:
            self.uploaded = Path(args[4]).read_text(encoding="ascii")
            return ""
        if args[:3] == ["git", "tag", "--list"]:
            return "v0.1.0\nv0.2.0\nllvm-22.1.8-1\n"
        raise AssertionError(args)

    def ssh_keygen(self, args, **kw):
        assert args[:3] == ["ssh-keygen", "-Y", "sign"] and args[args.index("-n") + 1] == signing.NAMESPACE, args
        path = Path(args[-1])
        Path(str(path) + ".sig").write_text(signing.sign(self.signer, path.read_bytes()), encoding="ascii")
        return subprocess.CompletedProcess(args, 0)


class SignReleaseTest(unittest.TestCase):
    def go(self, fake, keys=None, argv=(TAG, "--key", "k")):
        keys = {TAG: [signing.ed25519_public(SEED)], "v0.1.0": None} if keys is None else keys
        with mock.patch.object(sign_release, "run", fake.run), \
                mock.patch.object(sign_release, "keys_at", lambda rev: keys[rev]), \
                mock.patch.object(sign_release.subprocess, "run", fake.ssh_keygen), \
                mock.patch.object(sign_release.shutil, "which", lambda t: t), \
                contextlib.redirect_stdout(io.StringIO()):
            return sign_release.main(list(argv))

    def files(self):
        return {"DemonsSoulsRecomp-0.2.0-windows.zip": b"zip", "DemonsSoulsRecomp-0.2.0-linux.tar.xz": b"tar",
                "DemonsSoulsRecomp-0.2.0-source.zip": b"src"}

    def test_signs_and_uploads(self):
        fake = Fake(self.files())
        self.assertEqual(self.go(fake), 0)
        self.assertEqual(signing.verify(fake.sums, fake.uploaded, [signing.ed25519_public(SEED)]),
                         signing.ed25519_public(SEED))

    def test_checks_the_version_before(self):
        # The players on 0.1.0 check this update with 0.1.0's list: a key not on it is refused before the upload.
        fake = Fake(self.files())
        with self.assertRaisesRegex(sign_release.Error, "at v0.1.0"):
            self.go(fake, keys={TAG: [signing.ed25519_public(SEED)], "v0.1.0": [signing.ed25519_public(OTHER)]})
        self.assertIsNone(fake.uploaded)

    def test_refusals(self):
        cases = [
            (Fake(self.files(), draft=False), None, "published already"),
            (Fake(self.files(), attested=False), None, "no attestation"),
            (Fake(self.files(), signer=OTHER), None, "does not check"),
            (Fake(self.files()), {TAG: [], "v0.1.0": None}, "lists no key"),
        ]
        for fake, keys, why in cases:
            with self.subTest(why), self.assertRaisesRegex(sign_release.Error, why):
                self.go(fake, keys)
            self.assertIsNone(fake.uploaded)

    def test_a_file_that_does_not_match(self):
        fake = Fake(self.files())
        fake.files["DemonsSoulsRecomp-0.2.0-windows.zip"] = b"swapped after the checksums"
        with self.assertRaisesRegex(sign_release.Error, "does not match"):
            self.go(fake)
        self.assertIsNone(fake.uploaded)

    def test_dry_run_signs_nothing(self):
        fake = Fake(self.files())
        self.assertEqual(self.go(fake, argv=(TAG, "--dry-run")), 0)
        self.assertIsNone(fake.uploaded)

    def test_release_before(self):
        with mock.patch.object(sign_release, "run", lambda args, **kw: "v0.1.0\nv0.1.9\nv0.10.0\nv0.2.0-rc.1\n"):
            self.assertEqual(sign_release.release_before("v0.2.0"), "v0.1.9")
            self.assertEqual(sign_release.release_before("v0.11.0"), "v0.10.0")
            self.assertIsNone(sign_release.release_before("v0.1.0"))


if __name__ == "__main__":
    unittest.main()
