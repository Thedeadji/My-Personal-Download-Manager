"""Tests for the User-Agent toggle: resolution + a UA-gated server round-trip."""
import os
import sys
import threading
import http.server
import socketserver

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.config import (Settings, resolve_user_agent,
                         DEFAULT_USER_AGENT, BROWSER_USER_AGENT)
from mpdm.http_client import build_session, probe
from mpdm.engine import Download, DownloadError


def test_resolve_user_agent():
    assert resolve_user_agent(None) == DEFAULT_USER_AGENT
    assert resolve_user_agent("") == DEFAULT_USER_AGENT
    assert resolve_user_agent("default") == DEFAULT_USER_AGENT
    assert resolve_user_agent("browser") == BROWSER_USER_AGENT
    assert resolve_user_agent("Chrome") == BROWSER_USER_AGENT
    assert resolve_user_agent("MyCrawler/2.0") == "MyCrawler/2.0"


def test_build_session_applies_user_agent():
    s = build_session(Settings(user_agent="Custom/9"))
    assert s.headers["User-Agent"] == "Custom/9"


def _start_ua_gated_server():
    """Serve 10 bytes only to clients whose UA contains 'Mozilla', else 403."""
    body = b"0123456789"

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _gated(self, write_body):
            if "Mozilla" not in self.headers.get("User-Agent", ""):
                self.send_response(403)
                self.end_headers()
                return
            rng = self.headers.get("Range")
            if rng:
                self.send_response(206)
                self.send_header("Content-Range", f"bytes 0-{len(body)-1}/{len(body)}")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(len(body)))
            else:
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if write_body:
                self.wfile.write(body)

        def do_GET(self):
            self._gated(True)

    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), H)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]


def test_browser_ua_unblocks_gated_server(tmp_path=None):
    httpd, port = _start_ua_gated_server()
    url = f"http://127.0.0.1:{port}/f.bin"
    try:
        # Honest UA -> rejected.
        rejected = False
        try:
            probe(url, Settings())
        except Exception:
            rejected = True
        assert rejected, "honest UA should have been refused by the gated server"

        # Browser UA -> allowed; full download succeeds.
        import tempfile
        dest = os.path.join(tempfile.mkdtemp(), "f.bin")
        s = Settings(user_agent=BROWSER_USER_AGENT, connections=2)
        path = Download(url, settings=s, dest_path=dest).start()
        assert os.path.getsize(path) == 10
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    import inspect
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all user-agent tests passed")
