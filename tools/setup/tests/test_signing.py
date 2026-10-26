"""Tests for the release signatures (signing.py, docs/105 section 7).

  cd tools/setup && python -m unittest discover -s tests
"""
import base64
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from des_setup import signing  # noqa: E402

# RFC 8032 section 7.1, tests 1-3: seed, public key, message, signature (Go's crypto/ed25519 gives the same).
RFC8032 = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24"
     "655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302a"
     "eeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed2"
     "8dc027beceea1ec40a"),
]

# Made by Windows' own OpenSSH 9.5 with a throwaway key (thrown away):
#   ssh-keygen -t ed25519 -N "" -C throwaway-test -f testkey
#   ssh-keygen -Y sign -f testkey -n demons-souls-recomp-release msg.txt
OPENSSH_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAsgCRmXmmBMor6JZ+U+6QbJNCzQBWqJn+cNOFZZpHH3 throwaway-test"
OPENSSH_MSG = b"abc123  DemonsSoulsRecomp-0.2.0-windows.zip\n"
OPENSSH_SIG = """-----BEGIN SSH SIGNATURE-----
U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgCyAJGZeaYEyivoln5T7pBsk0LN
AFaomf5w04VlmkcfcAAAAbZGVtb25zLXNvdWxzLXJlY29tcC1yZWxlYXNlAAAAAAAAAAZz
aGE1MTIAAABTAAAAC3NzaC1lZDI1NTE5AAAAQDLNpNWGckwUOYhuQ5x29SQ9Vt2fEnRRCJ
COqSs2fPR+nHlIwlj5URErWIJi/DOeqH/xjP37NfVj25eXv1JJPQ4=
-----END SSH SIGNATURE-----
"""

SEED = bytes(range(32))
OTHER = bytes(range(1, 33))


def key_line(seed, comment="test"):
    return f"ssh-ed25519 {base64.b64encode(signing.key_blob(signing.ed25519_public(seed))).decode()} {comment}"


class Ed25519Test(unittest.TestCase):
    def test_rfc8032(self):
        for seed, pub, msg, sig in RFC8032:
            seed, pub, msg, sig = (bytes.fromhex(x) for x in (seed, pub, msg, sig))
            self.assertEqual(signing.ed25519_public(seed), pub)
            self.assertEqual(signing.ed25519_sign(seed, msg), sig)
            self.assertTrue(signing.ed25519_verify(pub, msg, sig))
            self.assertFalse(signing.ed25519_verify(pub, msg + b"x", sig))
            self.assertFalse(signing.ed25519_verify(pub, msg, sig[:-1] + bytes([sig[-1] ^ 1])))
            self.assertFalse(signing.ed25519_verify(pub, msg, sig[:32] + bytes([sig[32] ^ 1]) + sig[33:]))

    def test_s_at_or_above_the_order(self):
        _seed, pub, msg, sig = (bytes.fromhex(x) for x in RFC8032[0])
        s = int.from_bytes(sig[32:], "little") + signing._L   # the same point, S not reduced: refused
        self.assertFalse(signing.ed25519_verify(pub, msg, sig[:32] + s.to_bytes(32, "little")))

    def test_bad_points(self):
        self.assertFalse(signing.ed25519_verify(b"\xff" * 32, b"", bytes(64)))
        self.assertFalse(signing.ed25519_verify(b"\x00" * 31, b"", bytes(64)))


class SshSigTest(unittest.TestCase):
    def test_openssh_signature(self):
        key = signing.parse_key_line(OPENSSH_KEY)
        self.assertEqual(signing.verify(OPENSSH_MSG, OPENSSH_SIG, [key]), key)
        with self.assertRaisesRegex(signing.BadSignature, "does not match"):
            signing.verify(OPENSSH_MSG.replace(b"0.2.0", b"0.2.1"), OPENSSH_SIG, [key])

    def test_round_trip_and_refusals(self):
        key, other = signing.ed25519_public(SEED), signing.ed25519_public(OTHER)
        msg = b"0123  DemonsSoulsRecomp-1.0.0-windows.zip\n"
        sig = signing.sign(SEED, msg)
        self.assertEqual(signing.verify(msg, sig, [other, key]), key)
        self.assertEqual(signing.verify(msg, sig.replace("\n", "\r\n"), [key]), key)
        self.assertEqual(signing.verify(msg, signing.sign(SEED, msg, hash_name="sha256"), [key]), key)
        with self.assertRaisesRegex(signing.BadSignature, "does not trust"):
            signing.verify(msg, sig, [other])
        with self.assertRaisesRegex(signing.BadSignature, "no release key"):
            signing.verify(msg, sig, [])
        with self.assertRaisesRegex(signing.BadSignature, "something else"):
            signing.verify(msg, signing.sign(SEED, msg, namespace="file"), [key])
        with self.assertRaisesRegex(signing.BadSignature, "does not match"):
            signing.verify(msg + b" ", sig, [key])
        for broken in ("", "-----BEGIN SSH SIGNATURE-----\n!!\n-----END SSH SIGNATURE-----\n",
                       signing.armor(b"SSHSIG\x00\x00\x00\x01\x00\x00\x00\x09cut"), sig.replace("SSH SIGNATURE", "X")):
            with self.assertRaises(signing.BadSignature):
                signing.verify(msg, broken, [key])

    def test_key_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "keys.txt"
            self.assertEqual(signing.trusted_keys(path), [])            # (no file: no key)
            path.write_text(f"# the release key\n\n{key_line(SEED)}\n   {key_line(OTHER, 'the next one')}\n")
            self.assertEqual(signing.trusted_keys(path), [signing.ed25519_public(SEED), signing.ed25519_public(OTHER)])
            path.write_text(f"{key_line(SEED)}\nssh-rsa AAAAB3NzaC1yc2E= someone\n")
            with self.assertRaisesRegex(ValueError, "keys.txt:2"):
                signing.trusted_keys(path)

    def test_the_shipped_list_reads(self):
        signing.trusted_keys()                                          # (comments, or keys: never an error)


if __name__ == "__main__":
    unittest.main()
