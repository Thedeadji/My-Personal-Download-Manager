"""Offline tests for AES-128 and encrypted-HLS parsing/planning (no network)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import aes128, hls
from mpdm.config import Settings
from mpdm.grabber import _hls_decrypt_plan


# --- AES-128 known-answer tests (NIST SP 800-38A F.2, FIPS-197) -------------

def test_aes128_single_block_fips197():
    key = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    ct = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    # IV=0, one block => CBC == ECB; disable unpad (not PKCS7 data).
    assert aes128.decrypt_cbc(key, b"\x00" * 16, ct, unpad=False) == pt


def test_aes128_cbc_multiblock_nist():
    key = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    iv = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    ct = bytes.fromhex(
        "7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e22229516"
        "3ff1caa1681fac09120eca307586e1a7"
    )
    pt = bytes.fromhex(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710"
    )
    assert aes128.decrypt_cbc(key, iv, ct, unpad=False) == pt


def test_pkcs7_unpad():
    assert aes128.pkcs7_unpad(b"hello" + b"\x0b" * 11) == b"hello"
    assert aes128.pkcs7_unpad(b"A" * 16 + b"\x10" * 16) == b"A" * 16  # full pad block
    # Invalid padding is left untouched (defensive).
    assert aes128.pkcs7_unpad(b"abc\x05\x05") == b"abc\x05\x05"
    assert aes128.pkcs7_unpad(b"") == b""


# --- HLS IV handling --------------------------------------------------------

def test_parse_iv():
    iv = hls._parse_iv("0x000102030405060708090A0B0C0D0E0F")
    assert iv == bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    assert len(iv) == 16


def test_segment_iv_defaults_to_sequence():
    seg = hls.MediaSegment(uri="s0.ts", seq=5, key=hls.SegmentKey(method="AES-128", uri="k"))
    assert hls.segment_iv(seg) == (5).to_bytes(16, "big")
    seg2 = hls.MediaSegment(
        uri="s1.ts", seq=5,
        key=hls.SegmentKey(method="AES-128", uri="k", iv=b"\x11" * 16),
    )
    assert hls.segment_iv(seg2) == b"\x11" * 16  # explicit IV wins


# --- Playlist parsing with encryption + key rotation ------------------------

ENC_PLAYLIST = """#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:6
#EXT-X-MEDIA-SEQUENCE:10
#EXT-X-KEY:METHOD=AES-128,URI="https://host/key1.bin"
#EXTINF:6.0,
seg10.ts
#EXTINF:6.0,
seg11.ts
#EXT-X-KEY:METHOD=AES-128,URI="https://host/key2.bin",IV=0x00000000000000000000000000000063
#EXTINF:6.0,
seg12.ts
#EXT-X-KEY:METHOD=NONE
#EXTINF:6.0,
seg13.ts
#EXT-X-ENDLIST
"""


def test_parse_media_tracks_keys_and_sequence():
    pl = hls.parse_media(ENC_PLAYLIST, "https://host/media.m3u8")
    assert pl.media_sequence == 10
    assert pl.is_encrypted
    assert [s.uri for s in pl.seg_infos] == [
        "https://host/seg10.ts", "https://host/seg11.ts",
        "https://host/seg12.ts", "https://host/seg13.ts",
    ]
    # Sequence numbers start at MEDIA-SEQUENCE and increment.
    assert [s.seq for s in pl.seg_infos] == [10, 11, 12, 13]
    # key1 covers seg10/seg11 (no IV), key2 covers seg12 (explicit IV), seg13 none.
    assert pl.seg_infos[0].key.uri == "https://host/key1.bin"
    assert pl.seg_infos[0].key.iv is None
    assert pl.seg_infos[2].key.uri == "https://host/key2.bin"
    assert pl.seg_infos[2].key.iv == (0x63).to_bytes(16, "big")
    assert pl.seg_infos[3].key is None


class _FakeResp:
    def __init__(self, content):
        self.content = content

    def raise_for_status(self):
        pass


class _FakeSession:
    """Returns a deterministic 16-byte key per URI, records fetch count."""
    def __init__(self):
        self.calls = 0

    def get(self, uri, **kw):
        self.calls += 1
        # Distinct key bytes per URI so caching is observable.
        return _FakeResp(bytes([hash(uri) & 0xFF]) * 16)


def test_hls_decrypt_plan_builds_keys_ivs_and_caches():
    pl = hls.parse_media(ENC_PLAYLIST, "https://host/media.m3u8")
    sess = _FakeSession()
    plan = _hls_decrypt_plan(pl, Settings(), sess)
    assert len(plan) == 4
    # seg10/seg11 share key1 -> IV from sequence number.
    assert plan[0][1] == (10).to_bytes(16, "big")
    assert plan[1][1] == (11).to_bytes(16, "big")
    assert plan[0][0] == plan[1][0]            # same cached key bytes
    # seg12 uses key2 with explicit IV.
    assert plan[2][1] == (0x63).to_bytes(16, "big")
    assert plan[2][0] != plan[0][0]            # different key
    # seg13 unencrypted.
    assert plan[3] is None
    # Two distinct key URIs => two fetches (key1 cached across seg10/seg11).
    assert sess.calls == 2


def test_hls_decrypt_plan_none_when_clear():
    clear = "#EXTM3U\n#EXTINF:6,\na.ts\n#EXTINF:6,\nb.ts\n#EXT-X-ENDLIST\n"
    pl = hls.parse_media(clear, "https://host/m.m3u8")
    assert _hls_decrypt_plan(pl, Settings(), _FakeSession()) is None


def test_sample_aes_not_supported():
    text = ('#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI="https://host/k.bin"\n'
            '#EXTINF:6,\nseg.ts\n#EXT-X-ENDLIST\n')
    pl = hls.parse_media(text, "https://host/m.m3u8")
    try:
        _hls_decrypt_plan(pl, Settings(), _FakeSession())
    except NotImplementedError as e:
        assert "SAMPLE-AES" in str(e)
    else:
        raise AssertionError("expected NotImplementedError for SAMPLE-AES")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all hls-crypto tests passed")
