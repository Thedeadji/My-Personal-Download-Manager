# shared/ — cross-language contract & test vectors

Material that both the Python reference and the C++ port must agree on. Keeping it
here (rather than inside either implementation) makes it the single source of
truth and lets the C++ port be validated against the exact behavior Python ships.

## Contents (filled in as the port progresses)

- **Native-messaging protocol** — 4-byte little-endian length prefix + UTF-8 JSON;
  message types: `download`, `ping`/`pong`, `accepted`, `progress`, `done`,
  `error`. Both native hosts (Python `mpdm/native_host.py`, future C++) implement
  this identically.
- **`.mpdm` sidecar format** — resume metadata (url, dest, size, accept_ranges,
  etag, per-segment progress, status).
- **`settings.json` schema** — persisted preferences.
- **Test vectors** — golden inputs/outputs reused to prove the C++ port matches:
  - AES-128 KATs (NIST/FIPS) — see `python/tests/test_hls_crypto.py`
  - DASH manifests (SegmentTemplate/List/Base) — see `python/tests/test_dash.py`
  - HLS encrypted playlists (key rotation, media-sequence IV)
  - Segment math + dynamic-split invariants — `test_engine.py`, `test_dynamic.py`
  - Batch/wildcard expansion, Google Drive URL parsing, rate parsing

For now these live in the Python `tests/` directory; they'll be copied/extracted
here as language-neutral fixtures when the C++ test suite is set up.
