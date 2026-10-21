"""High-level stream grabber: HLS/DASH manifest -> downloaded MP4.

Ties the manifest parsers (hls, dash), the segment downloader and the muxer
together. Segments are fetched concurrently (each is a normal HTTP GET) and
then assembled with ffmpeg.
"""
from __future__ import annotations

import os
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable, Optional

import requests

from .config import Settings
from .http_client import build_session
from . import hls, dash, muxer, aes128


ProgressCb = Callable[[int, int], None]  # (segments_done, segments_total)

# One entry per segment: (key_bytes, iv_bytes) to AES-128-CBC decrypt, or None.
KeyIv = Optional[tuple]


def _download_one(url: str, path: str, settings: Settings, session: requests.Session,
                  key_iv: KeyIv = None) -> str:
    attempt = 0
    while True:
        try:
            r = session.get(url, stream=True, timeout=(settings.connect_timeout, settings.read_timeout),
                            allow_redirects=True, verify=settings.verify_tls)
            r.raise_for_status()
            with open(path, "wb") as f:
                for chunk in r.iter_content(settings.chunk_size):
                    if chunk:
                        f.write(chunk)
            r.close()
            if key_iv is not None:
                key, iv = key_iv
                with open(path, "rb") as f:
                    data = f.read()
                with open(path, "wb") as f:
                    f.write(aes128.decrypt_cbc(key, iv, data))
            return path
        except (requests.RequestException, OSError):
            attempt += 1
            if attempt > settings.max_retries:
                raise


def _fetch_segments(seg_urls: list[str], workdir: str, settings: Settings,
                    session: requests.Session, progress_cb: Optional[ProgressCb],
                    decrypt_plan: Optional[list] = None) -> list[str]:
    paths = [os.path.join(workdir, f"seg_{i:06d}") for i in range(len(seg_urls))]
    done = 0
    total = len(seg_urls)
    with ThreadPoolExecutor(max_workers=min(settings.connections, max(1, total))) as pool:
        futs = {}
        for i, (u, p) in enumerate(zip(seg_urls, paths)):
            key_iv = decrypt_plan[i] if decrypt_plan else None
            futs[pool.submit(_download_one, u, p, settings, session, key_iv)] = p
        for fut in as_completed(futs):
            fut.result()
            done += 1
            if progress_cb:
                progress_cb(done, total)
    return paths


def _hls_decrypt_plan(media, settings: Settings, session: requests.Session) -> Optional[list]:
    """Build a per-segment (key, iv) plan for an HLS media playlist.

    Returns None when nothing is encrypted. Raises NotImplementedError for
    encryption methods other than AES-128 (e.g. SAMPLE-AES).
    """
    if not media.is_encrypted:
        return None
    for seg in media.seg_infos:
        if seg.key and seg.key.method not in ("NONE", "", "AES-128"):
            raise NotImplementedError(
                f"HLS encryption method {seg.key.method!r} is not supported "
                "(only AES-128); SAMPLE-AES remains an extension point in hls.py"
            )
    key_cache: dict[str, bytes] = {}
    plan: list = []
    for seg in media.seg_infos:
        if not seg.key or seg.key.method in ("NONE", ""):
            plan.append(None)
            continue
        if not seg.key.uri:
            raise ValueError("encrypted segment has no key URI")
        key = key_cache.get(seg.key.uri)
        if key is None:
            key = hls.fetch_key(seg.key.uri, settings, session)
            key_cache[seg.key.uri] = key
        plan.append((key, hls.segment_iv(seg)))
    return plan


def grab(url: str, out_path: str, settings: Optional[Settings] = None,
         kind: str = "auto", prefer: str = "best",
         progress_cb: Optional[ProgressCb] = None) -> str:
    """Download an HLS or DASH stream to a single MP4 at out_path.

    kind: "hls", "dash", or "auto" (guess from the URL/extension).
    prefer: "best", "worst", or a resolution hint like "720".
    """
    settings = settings or Settings()
    session = build_session(settings)

    if kind == "auto":
        low = url.lower()
        kind = "dash" if ".mpd" in low else "hls"

    workdir = tempfile.mkdtemp(prefix="mdm_grab_")
    init_path = None
    decrypt_plan = None
    try:
        if kind == "hls":
            media, media_url = hls.resolve_media_playlist(url, settings, session, prefer=prefer)
            if not media.segments:
                raise ValueError("no media segments found in HLS playlist")
            # AES-128 encrypted playlists are decrypted per segment (keys may
            # rotate); unsupported methods raise from _hls_decrypt_plan.
            decrypt_plan = _hls_decrypt_plan(media, settings, session)
            seg_urls = media.segments
            init_url = media.init_segment
        elif kind == "dash":
            stream = dash.parse_mpd(url, settings, session, prefer=prefer)
            seg_urls = stream.segment_urls
            init_url = stream.init_url
            if not seg_urls:
                raise ValueError("no segments found in MPD")
        else:
            raise ValueError(f"unknown kind: {kind}")

        if init_url:
            init_path = os.path.join(workdir, "init.mp4")
            _download_one(init_url, init_path, settings, session)

        seg_paths = _fetch_segments(seg_urls, workdir, settings, session, progress_cb,
                                    decrypt_plan=decrypt_plan)
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        muxer.assemble_segments(seg_paths, out_path, init_path=init_path, cleanup=True)
        return out_path
    finally:
        # Clean the temp workdir (segments already removed by assemble on success).
        try:
            for name in os.listdir(workdir):
                os.remove(os.path.join(workdir, name))
            os.rmdir(workdir)
        except OSError:
            pass
