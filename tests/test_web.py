"""Web dashboard API tests — isolated HOME, ephemeral port, no network downloads."""
import json
import os
import sys
import tempfile
import threading
import urllib.request

# Isolate state_dir() (downloads.json / schedule.json) into a temp HOME.
_TMP = tempfile.mkdtemp()
os.environ["HOME"] = _TMP
os.environ["USERPROFILE"] = _TMP

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import web


def _req(base, path, body=None):
    url = base + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.status, r.read()


def run():
    httpd = web.serve(host="127.0.0.1", port=0, open_browser=False)
    base = f"http://127.0.0.1:{httpd.server_address[1]}"
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    try:
        # Home page renders.
        code, html = _req(base, "/")
        assert code == 200 and b"MyPersonalDownloadManager" in html

        # Empty state.
        code, body = _req(base, "/api/state")
        st = json.loads(body)
        assert st["downloads"] == [] and "global_rate_bps" in st

        # Add a (non-starting) download so no network is touched.
        code, body = _req(base, "/api/add",
                          {"url": "http://127.0.0.1:1/none", "start": False, "rate": "500k"})
        st = json.loads(body)
        assert len(st["downloads"]) == 1
        item = st["downloads"][0]
        assert item["status"] == "paused"
        assert item["max_rate_bps"] == 500 * 1024
        iid = item["id"]

        # Per-item rate change.
        code, body = _req(base, "/api/item_rate", {"id": iid, "rate": "1m"})
        st = json.loads(body)
        assert st["downloads"][0]["max_rate_bps"] == 1024 * 1024

        # Global rate.
        code, body = _req(base, "/api/global_rate", {"rate": "2m"})
        st = json.loads(body)
        assert st["global_rate_bps"] == 2 * 1024 * 1024

        # Remove.
        code, body = _req(base, "/api/remove", {"id": iid})
        st = json.loads(body)
        assert st["downloads"] == []

        print("ok web api: home, state, add, item_rate, global_rate, remove")
    finally:
        httpd.shutdown()
        web.Handler.manager.stop_scheduler()
        httpd.server_close()
    print("all web tests passed")


if __name__ == "__main__":
    run()
