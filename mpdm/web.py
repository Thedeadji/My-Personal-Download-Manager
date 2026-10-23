"""A small web dashboard for MPDM — a local browser UI over the same engine.

Dependency-free: built on the standard library's http.server, exposing a tiny
JSON API over a DownloadManager plus one self-contained HTML page. It binds to
localhost only (no auth, single-user, like the desktop GUI).

    python -m mpdm.web            # serve on http://127.0.0.1:8765 and open a browser
    python -m mpdm.web --port 9000 --no-open

The API:
    GET  /api/state                 -> {downloads:[...], global_rate_bps, max_concurrent}
    POST /api/add       {url, filename?, connections?, rate?, start?}
    POST /api/pause     {id}
    POST /api/resume    {id}
    POST /api/remove    {id, delete?}
    POST /api/item_rate {id, rate}
    POST /api/global_rate {rate}
    POST /api/pause_all | /api/resume_all | /api/clear
"""
from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .config import Settings
from .manager import DownloadManager
from .ratelimit import parse_rate


def _item_dict(it) -> dict:
    return {
        "id": it.id,
        "url": it.url,
        "filename": it.filename or "",
        "size": it.size,
        "downloaded": it.downloaded,
        "percent": it.percent,
        "speed": it.speed,
        "eta": it.eta,
        "status": it.status,
        "category": it.category,
        "max_rate_bps": it.max_rate_bps,
        "dest_path": it.dest_path,
        "error": it.error,
    }


class Handler(BaseHTTPRequestHandler):
    manager: DownloadManager = None  # set before the server starts

    def log_message(self, *a):  # keep the console quiet
        pass

    # ---- helpers ---------------------------------------------------------
    def _send(self, code, body: bytes, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj).encode("utf-8"))

    def _body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0:
                return {}
            return json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except (ValueError, json.JSONDecodeError):
            return {}

    def _state(self) -> dict:
        m = self.manager
        return {
            "downloads": [_item_dict(i) for i in m.ordered()],
            "global_rate_bps": m.global_rate_bps,
            "max_concurrent": m.max_concurrent,
            "output_dir": m.settings.output_dir,
        }

    # ---- routing ---------------------------------------------------------
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send(200, PAGE.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/api/state":
            self._json(self._state())
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = self._body()
        m = self.manager
        try:
            if path == "/api/add":
                url = (body.get("url") or "").strip()
                if not url:
                    return self._json({"error": "no url"}, 400)
                # Expand batch/wildcard patterns (img[1-100].jpg, lists) into many.
                m.add_batch(url,
                            connections=body.get("connections") or None,
                            max_rate_bps=parse_rate(body.get("rate")),
                            start=body.get("start", True))
            elif path == "/api/pause":
                m.pause(body.get("id"))
            elif path == "/api/resume":
                m.resume(body.get("id"))
            elif path == "/api/remove":
                m.remove(body.get("id"), delete_file=bool(body.get("delete")))
            elif path == "/api/item_rate":
                m.set_item_rate(body.get("id"), parse_rate(body.get("rate")))
            elif path == "/api/global_rate":
                m.set_global_rate(parse_rate(body.get("rate")))
            elif path == "/api/pause_all":
                m.pause_all()
            elif path == "/api/resume_all":
                m.resume_all()
            elif path == "/api/clear":
                m.clear_finished()
            else:
                return self._json({"error": "not found"}, 404)
        except Exception as e:  # noqa: BLE001
            return self._json({"error": str(e)}, 500)
        self._json(self._state())


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>MPDM — Web</title>
<style>
  :root { color-scheme: light dark; --bg:#0f1115; --card:#181b22; --line:#2a2f3a;
          --fg:#e6e8ee; --mut:#8b93a7; --accent:#1769ff; --ok:#2ea043; --bad:#e5534b; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 system-ui, -apple-system, Segoe UI, Roboto, sans-serif; }
  header { padding:16px 20px; border-bottom:1px solid var(--line); display:flex;
           gap:12px; align-items:center; flex-wrap:wrap; }
  h1 { font-size:16px; margin:0 12px 0 0; }
  input, button, select { font:inherit; }
  input[type=text], input[type=number] { background:var(--card); color:var(--fg);
    border:1px solid var(--line); border-radius:8px; padding:8px 10px; }
  #url { flex:1 1 320px; min-width:180px; }
  button { background:var(--accent); color:#fff; border:none; border-radius:8px;
    padding:8px 12px; cursor:pointer; }
  button.ghost { background:transparent; border:1px solid var(--line); color:var(--fg); }
  button:disabled { opacity:.4; cursor:default; }
  .bar2 { padding:10px 20px; display:flex; gap:8px; align-items:center; flex-wrap:wrap;
          border-bottom:1px solid var(--line); }
  .spacer { flex:1; }
  main { padding:12px 20px 40px; }
  table { width:100%; border-collapse:collapse; }
  th, td { text-align:left; padding:10px 8px; border-bottom:1px solid var(--line);
           vertical-align:middle; }
  th { color:var(--mut); font-weight:600; font-size:12px; }
  .name { max-width:360px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .prog { height:8px; background:var(--line); border-radius:6px; overflow:hidden; width:120px; }
  .prog > i { display:block; height:100%; background:var(--accent); width:0; }
  .st { font-size:12px; padding:2px 8px; border-radius:999px; background:var(--line); }
  .st.downloading { color:#9cf; } .st.completed { color:var(--ok); }
  .st.error { color:var(--bad); } .st.paused { color:#f0c674; }
  .row-actions button { padding:5px 9px; margin-right:4px; }
  .muted { color:var(--mut); }
  .err { color:var(--bad); font-size:12px; }
  footer { color:var(--mut); padding:0 20px 24px; }
</style>
</head>
<body>
<header>
  <h1>MyPersonalDownloadManager</h1>
  <input id="url" type="text" placeholder="Paste a URL (file, .m3u8, .mpd)…" />
  <input id="conns" type="number" min="1" max="32" value="8" title="connections" style="width:70px"/>
  <input id="rate" type="text" placeholder="limit e.g. 500k" style="width:110px" title="per-download speed limit"/>
  <button id="add">Add</button>
</header>
<div class="bar2">
  <button class="ghost" id="pauseAll">Pause all</button>
  <button class="ghost" id="resumeAll">Resume all</button>
  <button class="ghost" id="clear">Clear finished</button>
  <span class="spacer"></span>
  <span class="muted">Global limit:</span>
  <input id="glim" type="text" placeholder="unlimited" style="width:110px"/>
  <button class="ghost" id="applyGlim">Apply</button>
</div>
<main>
  <table>
    <thead><tr>
      <th>Name</th><th>Size</th><th>Progress</th><th>Status</th>
      <th>Speed</th><th>Time left</th><th>Category</th><th></th>
    </tr></thead>
    <tbody id="rows"></tbody>
  </table>
  <p id="empty" class="muted" style="padding:20px 4px">No downloads yet. Paste a URL above.</p>
</main>
<footer id="foot"></footer>
<script>
const $ = (s)=>document.querySelector(s);
function human(n){ if(n==null) return "—"; n=+n; const u=["B","KB","MB","GB","TB"];
  let i=0; while(Math.abs(n)>=1024&&i<u.length-1){n/=1024;i++;} return (i?n.toFixed(1):n.toFixed(0))+u[i]; }
function htime(s){ if(s==null) return "—"; s=Math.round(s); const h=(s/3600|0), m=((s%3600)/60|0), x=s%60;
  return h?`${h}h ${m}m`: m?`${m}m ${x}s`: `${x}s`; }
async function api(path, body){
  const r = await fetch(path, {method: body?"POST":"GET",
    headers: body?{"Content-Type":"application/json"}:{}, body: body?JSON.stringify(body):undefined});
  return r.json();
}
function render(state){
  const rows = $("#rows"); const list = state.downloads || [];
  $("#empty").style.display = list.length? "none":"block";
  rows.innerHTML = "";
  for(const d of list){
    const tr = document.createElement("tr");
    const pct = d.percent==null? null : Math.min(100, d.percent);
    const dl = (d.status==="paused"||d.status==="error");
    const pausable = (d.status==="downloading"||d.status==="queued");
    tr.innerHTML = `
      <td class="name" title="${d.url}">${d.filename || d.url}</td>
      <td>${human(d.size)}</td>
      <td><div class="prog"><i style="width:${pct==null?0:pct}%"></i></div>
          <span class="muted">${pct==null?"—":pct.toFixed(0)+"%"}</span></td>
      <td><span class="st ${d.status}">${d.status}</span></td>
      <td>${d.status==="downloading"&&d.speed? human(d.speed)+"/s":"—"}</td>
      <td>${d.status==="downloading"? htime(d.eta):"—"}</td>
      <td class="muted">${d.category||""}</td>
      <td class="row-actions" style="white-space:nowrap">
        <button data-a="resume" ${dl?"":"disabled"}>Resume</button>
        <button data-a="pause" ${pausable?"":"disabled"}>Stop</button>
        <button class="ghost" data-a="limit">Limit</button>
        <button class="ghost" data-a="remove">✕</button>
        ${d.error?`<div class="err">${d.error}</div>`:""}
      </td>`;
    tr.querySelectorAll("button").forEach(b=> b.addEventListener("click", ()=>act(b.dataset.a, d)));
    rows.appendChild(tr);
  }
  $("#foot").textContent =
    `${list.length} downloads · ${list.filter(x=>x.status==="downloading").length} active · ` +
    `global limit: ${state.global_rate_bps? human(state.global_rate_bps)+"/s":"unlimited"} · saving to ${state.output_dir}`;
}
async function act(a, d){
  if(a==="resume") render(await api("/api/resume",{id:d.id}));
  else if(a==="pause") render(await api("/api/pause",{id:d.id}));
  else if(a==="remove"){ const del = confirm("Delete the file from disk too? OK = delete, Cancel = just remove from list");
    render(await api("/api/remove",{id:d.id, delete:del})); }
  else if(a==="limit"){ const v = prompt("Speed limit for this download (e.g. 500k, blank = unlimited):", "");
    if(v!==null) render(await api("/api/item_rate",{id:d.id, rate:v})); }
}
$("#add").addEventListener("click", async ()=>{
  const url = $("#url").value.trim(); if(!url) return;
  render(await api("/api/add",{url, connections:+$("#conns").value||8, rate:$("#rate").value}));
  $("#url").value="";
});
$("#url").addEventListener("keydown", e=>{ if(e.key==="Enter") $("#add").click(); });
$("#pauseAll").addEventListener("click", async ()=> render(await api("/api/pause_all",{})));
$("#resumeAll").addEventListener("click", async ()=> render(await api("/api/resume_all",{})));
$("#clear").addEventListener("click", async ()=> render(await api("/api/clear",{})));
$("#applyGlim").addEventListener("click", async ()=> render(await api("/api/global_rate",{rate:$("#glim").value})));
async function poll(){ try{ render(await api("/api/state")); }catch(e){} setTimeout(poll, 1000); }
poll();
</script>
</body>
</html>"""


def serve(host="127.0.0.1", port=8765, open_browser=True) -> ThreadingHTTPServer:
    Handler.manager = DownloadManager(settings=Settings(), max_concurrent=3)
    Handler.manager.start_scheduler()
    httpd = ThreadingHTTPServer((host, port), Handler)
    url = f"http://{host}:{port}/"
    print(f"MPDM web UI on {url}  (Ctrl+C to stop)")
    if open_browser:
        import webbrowser
        threading.Thread(target=lambda: webbrowser.open(url), daemon=True).start()
    return httpd


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="mpdm.web", description="MPDM web dashboard")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-open", action="store_true", help="don't open a browser")
    args = ap.parse_args(argv)
    httpd = serve(args.host, args.port, open_browser=not args.no_open)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping…")
    finally:
        Handler.manager.stop_scheduler()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
