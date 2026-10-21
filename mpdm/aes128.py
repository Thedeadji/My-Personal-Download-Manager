"""Self-contained AES-128 (CBC) decryption — zero dependencies.

HLS AES-128 (RFC 8216 §5.2) encrypts each media segment as a single unit with
AES-128 in CBC mode and PKCS#7 padding. Python's standard library ships no block
cipher, and this project's core deliberately depends only on the stdlib and
`requests`, so this module implements AES-128 from scratch — readable over
clever — purely to decrypt downloaded segments.

A fast path is used automatically when a real crypto library happens to be
installed (`cryptography`, then `pycryptodome`); otherwise the pure-Python
implementation below is used. The pure path is correct but not fast — fine for a
learning/research tool, and it keeps MPDM dependency-free by default.

Only decryption is needed (we never encrypt), so only the inverse cipher is
implemented.
"""
from __future__ import annotations

# --- AES tables ------------------------------------------------------------

# Inverse S-box (used by the decryption round's SubBytes step).
_INV_SBOX = bytes.fromhex(
    "52096ad53036a538bf40a39e81f3d7fb"
    "7ce339829b2fff87348e4344c4dee9cb"
    "547b9432a6c2233dee4c950b42fac34e"
    "082ea16628d924b2765ba2496d8bd125"
    "72f8f66486689816d4a45ccc5d65b692"
    "6c704850fdedb9da5e154657a78d9d84"
    "90d8ab008cbcd30af7e45805b8b34506"
    "d02c1e8fca3f0f02c1afbd0301138a6b"
    "3a9111414f67dcea97f2cfcef0b4e673"
    "96ac7422e7ad3585e2f937e81c75df6e"
    "47f11a711d29c5896fb7620eaa18be1b"
    "fc563e4bc6d279209adbc0fe78cd5af4"
    "1fdda8338807c731b11210592780ec5f"
    "60517fa919b54a0d2de57a9f93c99cef"
    "a0e03b4dae2af5b0c8ebbb3c83539961"
    "172b047eba77d626e169146355210c7d"
)

# Round constants for key expansion.
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)

# Forward S-box — needed only inside key expansion (SubWord).
_SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76"
    "ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d83115"
    "04c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f84"
    "53d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa8"
    "51a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d1973"
    "60814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479"
    "e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a"
    "703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df"
    "8ca1890dbfe6426841992d0fb054bb16"
)


def _xtime(a: int) -> int:
    """Multiply by 2 in GF(2^8)."""
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF


def _mul(a: int, b: int) -> int:
    """Multiply two bytes in GF(2^8)."""
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        b >>= 1
        a = _xtime(a)
    return p & 0xFF


def _key_expansion(key: bytes) -> list[list[int]]:
    """Expand a 16-byte key into 11 round keys (each 16 bytes)."""
    if len(key) != 16:
        raise ValueError("AES-128 key must be 16 bytes")
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]  # 4 words of 4 bytes
    for i in range(4, 44):
        temp = list(words[i - 1])
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]                      # RotWord
            temp = [_SBOX[b] for b in temp]                 # SubWord
            temp[0] ^= _RCON[i // 4 - 1]
        words.append([words[i - 4][j] ^ temp[j] for j in range(4)])
    # Group the 44 words into 11 round keys of 16 bytes.
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]


def _inv_sub_bytes(state: list[int]) -> None:
    for i in range(16):
        state[i] = _INV_SBOX[state[i]]


def _inv_shift_rows(state: list[int]) -> None:
    # State is column-major: index = row + 4*col. Rotate each row right by its index.
    new = state[:]
    for row in range(4):
        for col in range(4):
            new[row + 4 * col] = state[row + 4 * ((col - row) % 4)]
    state[:] = new


def _inv_mix_columns(state: list[int]) -> None:
    for c in range(4):
        i = 4 * c
        a0, a1, a2, a3 = state[i], state[i + 1], state[i + 2], state[i + 3]
        state[i]     = _mul(a0, 14) ^ _mul(a1, 11) ^ _mul(a2, 13) ^ _mul(a3, 9)
        state[i + 1] = _mul(a0, 9)  ^ _mul(a1, 14) ^ _mul(a2, 11) ^ _mul(a3, 13)
        state[i + 2] = _mul(a0, 13) ^ _mul(a1, 9)  ^ _mul(a2, 14) ^ _mul(a3, 11)
        state[i + 3] = _mul(a0, 11) ^ _mul(a1, 13) ^ _mul(a2, 9)  ^ _mul(a3, 14)


def _add_round_key(state: list[int], rk: list[int]) -> None:
    for i in range(16):
        state[i] ^= rk[i]


def _decrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    """Decrypt one 16-byte block (state is column-major, as in FIPS-197)."""
    state = list(block)
    _add_round_key(state, round_keys[10])
    for rnd in range(9, 0, -1):
        _inv_shift_rows(state)
        _inv_sub_bytes(state)
        _add_round_key(state, round_keys[rnd])
        _inv_mix_columns(state)
    _inv_shift_rows(state)
    _inv_sub_bytes(state)
    _add_round_key(state, round_keys[0])
    return bytes(state)


def _cbc_decrypt_pure(key: bytes, iv: bytes, data: bytes) -> bytes:
    if len(iv) != 16:
        raise ValueError("IV must be 16 bytes")
    if len(data) % 16 != 0:
        raise ValueError("ciphertext length must be a multiple of 16")
    round_keys = _key_expansion(key)
    out = bytearray()
    prev = iv
    for off in range(0, len(data), 16):
        block = data[off:off + 16]
        plain = _decrypt_block(block, round_keys)
        out.extend(b ^ p for b, p in zip(plain, prev))
        prev = block
    return bytes(out)


# --- Backends --------------------------------------------------------------

def _cbc_decrypt_cryptography(key: bytes, iv: bytes, data: bytes) -> bytes:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return dec.update(data) + dec.finalize()


def _cbc_decrypt_pycryptodome(key: bytes, iv: bytes, data: bytes) -> bytes:
    from Crypto.Cipher import AES
    return AES.new(key, AES.MODE_CBC, iv).decrypt(data)


def _select_backend():
    try:
        import cryptography  # noqa: F401
        return _cbc_decrypt_cryptography
    except Exception:
        pass
    try:
        import Crypto  # noqa: F401
        return _cbc_decrypt_pycryptodome
    except Exception:
        pass
    return _cbc_decrypt_pure


_BACKEND = _select_backend()


def pkcs7_unpad(data: bytes) -> bytes:
    """Remove PKCS#7 padding; return data unchanged if padding looks invalid."""
    if not data:
        return data
    pad = data[-1]
    if 1 <= pad <= 16 and len(data) >= pad and all(b == pad for b in data[-pad:]):
        return data[:-pad]
    return data


def decrypt_cbc(key: bytes, iv: bytes, data: bytes, unpad: bool = True) -> bytes:
    """AES-128-CBC decrypt `data`; strip PKCS#7 padding by default.

    Uses an installed crypto library if available, else the pure-Python path.
    """
    if len(key) != 16:
        raise ValueError("AES-128 key must be 16 bytes")
    plain = _BACKEND(key, iv, data)
    return pkcs7_unpad(plain) if unpad else plain


def backend_name() -> str:
    return {
        _cbc_decrypt_cryptography: "cryptography",
        _cbc_decrypt_pycryptodome: "pycryptodome",
        _cbc_decrypt_pure: "pure-python",
    }.get(_BACKEND, "unknown")
