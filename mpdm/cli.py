"""Command-line interface for MyPersonalDownloadManager."""
from __future__ import annotations

import argparse
import sys
import time

from . import __app_name__, __app_short__, __version__
from .config import Settings
from .models import Progress, Status
from .engine import Download
from . import grabber


def _human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024:
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}PB"


def _human_time(s: float) -> str:
    s = int(s)
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m"
    if m:
        return f"{m}m{sec:02d}s"
    return f"{sec}s"


def _make_bar(p: Progress, width: int = 28) -> str:
    pct = p.percent
    if pct is None:
        return f"[{'?' * width}]"
    filled = int(width * pct / 100)
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def _progress_printer():
    def cb(p: Progress):
        if p.status in (Status.PROBING, Status.PENDING):
            sys.stdout.write(f"\r{p.status.value}... ")
            sys.stdout.flush()
            return
        bar = _make_bar(p)
        pct = f"{p.percent:5.1f}%" if p.percent is not None else "  ?  "
        total = _human(p.total) if p.total else "?"
        eta = _human_time(p.eta_seconds) if p.eta_seconds is not None else "?"
        line = (f"\r{bar} {pct}  {_human(p.downloaded)}/{total}  "
                f"{_human(p.speed_bps)}/s  {p.active_segments} conn  ETA {eta}   ")
        sys.stdout.write(line)
        sys.stdout.flush()
        if p.status in (Status.COMPLETED, Status.FAILED):
            sys.stdout.write("\n")
    return cb


def cmd_get(args) -> int:
    from .ratelimit import parse_rate
    from .config import resolve_user_agent
    settings = Settings.from_env()
    if args.connections:
        settings.connections = args.connections
    if args.output_dir:
        settings.output_dir = args.output_dir
    if args.ua:
        settings.user_agent = resolve_user_agent(args.ua)
    if args.limit:
        try:
            settings.max_rate_bps = parse_rate(args.limit)
        except ValueError as e:
            print(f"Error: {e}", file=sys.stderr)
            return 2
    cb = None if args.quiet else _progress_printer()
    dl = Download(args.url, settings=settings, dest_path=args.output, progress_cb=cb)
    t0 = time.time()
    try:
        path = dl.start()
    except Exception as e:  # noqa: BLE001
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    print(f"Saved to {path}  ({_human_time(time.time() - t0)})")
    return 0


def cmd_grab(args) -> int:
    from .config import resolve_user_agent
    settings = Settings.from_env()
    if args.connections:
        settings.connections = args.connections
    if args.ua:
        settings.user_agent = resolve_user_agent(args.ua)

    last = {"n": 0}

    def cb(done, total):
        if args.quiet:
            return
        pct = done * 100 / total if total else 0
        sys.stdout.write(f"\rFetching segments {done}/{total} ({pct:4.1f}%)   ")
        sys.stdout.flush()
        last["n"] = done

    try:
        out = grabber.grab(args.url, args.output, settings=settings,
                           kind=args.type, prefer=args.quality, progress_cb=cb)
    except Exception as e:  # noqa: BLE001
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    if not args.quiet:
        sys.stdout.write("\n")
    print(f"Saved stream to {out}")
    return 0


def cmd_clean(args) -> int:
    from . import storage
    settings = Settings.from_env()
    target = args.output_dir or settings.output_dir
    res = storage.clean_dir(target, purge_partials=args.purge)
    print(f"Cleaned {target}:")
    print(f"  resume sidecars removed: {len(res['sidecars'])}")
    print(f"  temp checkpoints removed: {len(res['tmp'])}")
    if args.purge:
        print(f"  incomplete files removed: {len(res['partials'])}")
        for p in res["partials"]:
            print(f"    - {p}")
    else:
        print("  (incomplete target files kept; re-run with --purge to delete them)")
    if res["kept_locked"]:
        print(f"  could not remove (locked/in use): {len(res['kept_locked'])}")
        for p in res["kept_locked"]:
            print(f"    ! {p}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="mpdm",
        description=f"{__app_name__} ({__app_short__}) {__version__} — a from-scratch download manager.",
    )
    p.add_argument("--version", action="version", version=f"{__app_name__} {__version__}")
    sub = p.add_subparsers(dest="command", required=True)

    g = sub.add_parser("get", help="download a file (multi-connection, resumable)")
    g.add_argument("url")
    g.add_argument("-o", "--output", help="output file path")
    g.add_argument("-d", "--output-dir", help="output directory")
    g.add_argument("-n", "--connections", type=int, help="number of parallel connections")
    g.add_argument("-l", "--limit", help='speed limit, e.g. "500k", "1.5m" (0/omit = unlimited)')
    g.add_argument("--ua", help='User-Agent: "browser" for a Chrome UA, or a custom string '
                                "(default: honest MPDM UA)")
    g.add_argument("-q", "--quiet", action="store_true")
    g.set_defaults(func=cmd_get)

    gr = sub.add_parser("grab", help="download an HLS/DASH stream to MP4 (needs ffmpeg)")
    gr.add_argument("url")
    gr.add_argument("-o", "--output", required=True, help="output .mp4 path")
    gr.add_argument("-t", "--type", choices=["auto", "hls", "dash"], default="auto")
    gr.add_argument("--quality", default="best", help='"best", "worst", or a hint like "720"')
    gr.add_argument("-n", "--connections", type=int, help="parallel segment downloads")
    gr.add_argument("--ua", help='User-Agent: "browser" for a Chrome UA, or a custom string')
    gr.add_argument("-q", "--quiet", action="store_true")
    gr.set_defaults(func=cmd_grab)

    cl = sub.add_parser("clean", help="remove orphaned resume sidecars (.mpdm) and temp files")
    cl.add_argument("-d", "--output-dir", help="directory to clean (default: your downloads dir)")
    cl.add_argument("--purge", action="store_true",
                    help="also delete the incomplete target files, not just sidecars")
    cl.set_defaults(func=cmd_clean)

    return p


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
