"""Release signatures (docs/105 section 7). Each release's SHA256SUMS is signed with the project's release key, offline,
by OpenSSH's `ssh-keygen -Y sign -n demons-souls-recomp-release` (tools/release/sign_release.py). An update is taken
only when its SHA256SUMS.sig checks against a key in release-keys.txt beside this file, the keys of the version that
is installed: a release cannot vouch for itself.

The signature is OpenSSH's SSHSIG (PROTOCOL.sshsig in OpenSSH's source): an Ed25519 signature over
"SSHSIG", the namespace, a reserved field, the hash's name and the hash of the file. Ed25519 (RFC 8032) is checked
here in plain Python, as setup's Python has none of its own. This checks public data only, so it need not run in
constant time. sign() is for the tests' made-up keys; the real key never comes near this code.
"""
import base64
import hashlib
import struct
from pathlib import Path

NAMESPACE = "demons-souls-recomp-release"
KEYS = Path(__file__).with_name("release-keys.txt")
SIG_SUFFIX = ".sig"
_MAGIC = b"SSHSIG"
_KEY_TYPE = "ssh-ed25519"
_HASHES = {"sha512": hashlib.sha512, "sha256": hashlib.sha256}
_ARMOR = ("-----BEGIN SSH SIGNATURE-----", "-----END SSH SIGNATURE-----")


class BadSignature(ValueError):
    """The signature is malformed, made with a key this build does not trust, or does not match."""


# ---- Ed25519 (RFC 8032 section 5.1) ----

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    """Two points in extended coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, x*y = T/Z."""
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    pa = (y1 - x1) * (y2 - x2) % _P
    pb = (y1 + x1) * (y2 + x2) % _P
    pc = 2 * t1 * t2 * _D % _P
    pd = 2 * z1 * z2 % _P
    e, f, g, h = pb - pa, pd - pc, pd + pc, pb + pa
    return e * f % _P, g * h % _P, f * g % _P, e * h % _P


def _mul(s, point):
    out = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            out = _add(out, point)
        point = _add(point, point)
        s >>= 1
    return out


def _same(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _x_of(y, odd):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if odd else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    return _P - x if (x & 1) != odd else x


def _decode(b: bytes):
    """A point from its 32 bytes, or None if they are not one."""
    if len(b) != 32:
        return None
    y = int.from_bytes(b, "little")
    odd, y = y >> 255, y & ((1 << 255) - 1)
    x = _x_of(y, odd)
    return None if x is None else (x, y, 1, x * y % _P)


def _encode(point) -> bytes:
    zi = pow(point[2], _P - 2, _P)
    x, y = point[0] * zi % _P, point[1] * zi % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


_BY = 4 * pow(5, _P - 2, _P) % _P
_BX = _x_of(_BY, 0)
_B = (_BX, _BY, 1, _BX * _BY % _P)


def _hint(b: bytes) -> int:
    return int.from_bytes(hashlib.sha512(b).digest(), "little") % _L


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    """RFC 8032's check: S below the group order, A and R points, [S]B = R + [k]A."""
    if len(public) != 32 or len(signature) != 64:
        return False
    a, r = _decode(public), _decode(signature[:32])
    s = int.from_bytes(signature[32:], "little")
    if a is None or r is None or s >= _L:
        return False
    k = _hint(signature[:32] + public + message)
    return _same(_mul(s, _B), _add(r, _mul(k, a)))


def _expand(seed: bytes):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a = (a & ((1 << 254) - 8)) | (1 << 254)
    return a, h[32:]


def ed25519_public(seed: bytes) -> bytes:
    return _encode(_mul(_expand(seed)[0], _B))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    """(Tests only.)"""
    a, prefix = _expand(seed)
    public = _encode(_mul(a, _B))
    r = _hint(prefix + message)
    rb = _encode(_mul(r, _B))
    s = (r + _hint(rb + public + message) * a) % _L
    return rb + s.to_bytes(32, "little")


# ---- OpenSSH's wire format and SSHSIG ----

def _string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def _strings(blob: bytes, n=None):
    """The length-prefixed fields of `blob`; all of them must fit exactly."""
    out, at = [], 0
    while at < len(blob) and (n is None or len(out) < n):
        if at + 4 > len(blob):
            raise BadSignature("a field is cut short")
        size = struct.unpack(">I", blob[at:at + 4])[0]
        if at + 4 + size > len(blob):
            raise BadSignature("a field is cut short")
        out.append(blob[at + 4:at + 4 + size])
        at += 4 + size
    if at != len(blob):
        raise BadSignature("there is more after the fields")
    return out


def key_blob(public: bytes) -> bytes:
    """An Ed25519 public key as OpenSSH writes it on the wire (and base64 in a .pub line)."""
    return _string(_KEY_TYPE.encode()) + _string(public)


def parse_key_line(line: str) -> bytes:
    """The 32-byte key of an OpenSSH public key line: "ssh-ed25519 AAAA... comment"."""
    w = line.split()
    if len(w) < 2 or w[0] != _KEY_TYPE:
        raise ValueError("not an ssh-ed25519 public key line")
    try:
        f = _strings(base64.b64decode(w[1], validate=True))
    except (ValueError, BadSignature) as e:
        raise ValueError("the key's base64 does not decode to a key") from e
    if len(f) != 2 or f[0] != _KEY_TYPE.encode() or len(f[1]) != 32:
        raise ValueError("not an Ed25519 key")
    return f[1]


def trusted_keys(path=None) -> list:
    """The release keys this build trusts: release-keys.txt's lines, comments and blank lines skipped. A line that is
    not a key is an error, so a typo cannot go unnoticed as "no key"."""
    path = KEYS if path is None else path
    try:
        text = Path(path).read_text(encoding="utf-8")
    except FileNotFoundError:
        return []
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if line.strip() and not line.lstrip().startswith("#"):
            try:
                out.append(parse_key_line(line))
            except ValueError as e:
                raise ValueError(f"{path}:{n}: {e}") from e
    return out


def _signed_data(namespace: bytes, reserved: bytes, hash_name: bytes, digest: bytes) -> bytes:
    return _MAGIC + _string(namespace) + _string(reserved) + _string(hash_name) + _string(digest)


def armor(blob: bytes) -> str:
    b = base64.b64encode(blob).decode()
    return "\n".join([_ARMOR[0]] + [b[i:i + 70] for i in range(0, len(b), 70)] + [_ARMOR[1]]) + "\n"


def sign(seed: bytes, message: bytes, namespace: str = NAMESPACE, hash_name: str = "sha512") -> str:
    """An armoured SSHSIG of `message`, as ssh-keygen -Y sign writes it (tests only)."""
    digest = _HASHES[hash_name](message).digest()
    signed = _signed_data(namespace.encode(), b"", hash_name.encode(), digest)
    sig = _string(_KEY_TYPE.encode()) + _string(ed25519_sign(seed, signed))
    blob = (_MAGIC + struct.pack(">I", 1) + _string(key_blob(ed25519_public(seed))) + _string(namespace.encode())
            + _string(b"") + _string(hash_name.encode()) + _string(sig))
    return armor(blob)


def verify(message: bytes, signature_text: str, keys=None, namespace: str = NAMESPACE) -> bytes:
    """Checks an armoured SSHSIG of `message`. Returns the key that signed it (one of `keys`, by default the
    trusted ones); raises BadSignature otherwise."""
    if keys is None:
        try:
            keys = trusted_keys()
        except ValueError as e:
            raise BadSignature(f"this build's release key list is broken: {e}") from e
    if not keys:
        raise BadSignature("this build has no release key to check updates with")
    lines = [x.strip() for x in str(signature_text).replace("\r\n", "\n").split("\n") if x.strip()]
    if len(lines) < 3 or lines[0] != _ARMOR[0] or lines[-1] != _ARMOR[1]:
        raise BadSignature("not an SSH signature")
    try:
        blob = base64.b64decode("".join(lines[1:-1]), validate=True)
    except ValueError as e:
        raise BadSignature("the signature's base64 does not decode") from e
    if not blob.startswith(_MAGIC) or len(blob) < len(_MAGIC) + 4:
        raise BadSignature("not an SSH signature")
    if struct.unpack(">I", blob[6:10])[0] != 1:
        raise BadSignature("an SSH signature of another version")
    f = _strings(blob[10:])
    if len(f) != 5:
        raise BadSignature("an SSH signature with other fields")
    pub_blob, ns, reserved, hash_name, sig_blob = f
    if ns != namespace.encode():
        raise BadSignature(f"a signature for something else ({ns.decode('utf-8', 'replace')!r})")
    h = _HASHES.get(hash_name.decode("ascii", "replace"))
    if h is None:
        raise BadSignature("a signature over an unknown hash")
    key = next((k for k in keys if key_blob(k) == pub_blob), None)
    if key is None:
        raise BadSignature("signed with a key this build does not trust")
    s = _strings(sig_blob)
    if len(s) != 2 or s[0] != _KEY_TYPE.encode() or len(s[1]) != 64:
        raise BadSignature("not an Ed25519 signature")
    if not ed25519_verify(key, _signed_data(ns, reserved, hash_name, h(message).digest()), s[1]):
        raise BadSignature("the signature does not match")
    return key
