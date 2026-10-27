#include "mpdm/web.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#include "mpdm/config.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/ratelimit.hpp"

using nlohmann::json;

namespace mpdm {

// The dashboard page (identical UI to the Python web.py PAGE).
static const char* PAGE = R"PAGE(<!doctype html>
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
</html>)PAGE";

static json item_to_json(const std::shared_ptr<DownloadItem>& it) {
    json j;
    j["id"] = it->id;
    j["url"] = it->url;
    j["filename"] = it->filename;
    j["size"] = it->size ? json(*it->size) : json(nullptr);
    j["downloaded"] = it->downloaded;
    auto pct = it->percent();
    j["percent"] = pct ? json(*pct) : json(nullptr);
    j["speed"] = it->speed;
    j["eta"] = it->eta ? json(*it->eta) : json(nullptr);
    j["status"] = it->status;
    j["category"] = it->category();
    j["max_rate_bps"] = it->max_rate_bps;
    j["dest_path"] = it->dest_path;
    j["error"] = it->error;
    return j;
}

static json state_json(DownloadManager& m) {
    json dl = json::array();
    for (auto& it : m.ordered()) dl.push_back(item_to_json(it));
    return {{"downloads", dl},
            {"global_rate_bps", m.global_rate_bps()},
            {"max_concurrent", m.max_concurrent},
            {"output_dir", m.settings.output_dir}};
}

// parse_rate on a possibly-missing/empty JSON value ("" -> 0, like the JS UI).
static long long rate_of(const json& body, const char* key = "rate") {
    if (!body.contains(key) || body[key].is_null()) return 0;
    std::string s = body[key].is_string() ? body[key].get<std::string>() : body[key].dump();
    return parse_rate(s);
}

static std::string id_of(const json& body) {
    if (body.contains("id") && body["id"].is_string()) return body["id"].get<std::string>();
    return "";
}

static void open_in_browser(const std::string& url) {
#ifdef _WIN32
    std::system(("start \"\" \"" + url + "\"").c_str());
#elif defined(__APPLE__)
    std::system(("open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#else
    std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

int run_web(const std::string& host, int port, bool open_browser) {
    DownloadManager manager(Settings{}, 3, 0);
    manager.start_scheduler();
    httplib::Server svr;

    auto send_json = [](httplib::Response& res, const json& j, int code = 200) {
        res.status = code;
        res.set_content(j.dump(), "application/json");
    };

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(PAGE, "text/html; charset=utf-8");
    });
    svr.Get("/index.html", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(PAGE, "text/html; charset=utf-8");
    });
    svr.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) {
        send_json(res, state_json(manager));
    });

    auto handle_post = [&](const httplib::Request& req, httplib::Response& res,
                           const std::string& path) {
        json body;
        if (!req.body.empty()) {
            try {
                body = json::parse(req.body);
            } catch (...) {
                body = json::object();
            }
        } else {
            body = json::object();
        }
        try {
            if (path == "/api/add") {
                std::string url = body.value("url", "");
                // trim
                size_t a = url.find_first_not_of(" \t\r\n");
                size_t b = url.find_last_not_of(" \t\r\n");
                url = (a == std::string::npos) ? "" : url.substr(a, b - a + 1);
                if (url.empty()) return send_json(res, {{"error", "no url"}}, 400);
                int conns = 0;
                if (body.contains("connections") && body["connections"].is_number())
                    conns = body["connections"].get<int>();
                bool start = body.value("start", true);
                manager.add_batch(url, conns, start, rate_of(body));
            } else if (path == "/api/pause") {
                manager.pause(id_of(body));
            } else if (path == "/api/resume") {
                manager.resume(id_of(body));
            } else if (path == "/api/remove") {
                manager.remove(id_of(body), body.value("delete", false));
            } else if (path == "/api/item_rate") {
                manager.set_item_rate(id_of(body), rate_of(body));
            } else if (path == "/api/global_rate") {
                manager.set_global_rate(static_cast<double>(rate_of(body)));
            } else if (path == "/api/pause_all") {
                manager.pause_all();
            } else if (path == "/api/resume_all") {
                manager.resume_all();
            } else if (path == "/api/clear") {
                manager.clear_finished();
            } else {
                return send_json(res, {{"error", "not found"}}, 404);
            }
        } catch (const std::exception& e) {
            return send_json(res, {{"error", e.what()}}, 500);
        }
        send_json(res, state_json(manager));
    };

    for (const char* p : {"/api/add", "/api/pause", "/api/resume", "/api/remove", "/api/item_rate",
                          "/api/global_rate", "/api/pause_all", "/api/resume_all", "/api/clear"}) {
        std::string path = p;
        svr.Post(path, [&handle_post, path](const httplib::Request& req, httplib::Response& res) {
            handle_post(req, res, path);
        });
    }

    std::string url = "http://" + host + ":" + std::to_string(port) + "/";
    std::cout << "MPDM web UI on " << url << "  (Ctrl+C to stop)\n";
    if (open_browser) open_in_browser(url);

    if (!svr.listen(host, port)) {
        std::cerr << "Error: could not bind " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}

} // namespace mpdm
