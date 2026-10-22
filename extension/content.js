// On-page "download this video" panel (IDM-style).
//
// Shows a small floating button when downloadable media is detected on the page
// — HTML5 <video>/<audio> sources plus HLS/DASH manifests and media files the
// background script sniffed from network traffic. Click it to pick an item and
// send it to MPDM. Fully isolated in a shadow root so page CSS can't touch it,
// and controlled by the "Show video download panel" setting.

(() => {
  if (window.__mpdmPanel) return;
  window.__mpdmPanel = true;

  let enabled = true;
  let host = null, shadow = null, fab = null, panel = null, listEl = null;
  let collapsed = true, hiddenForPage = false, items = [];

  function send(msg, cb) {
    try { chrome.runtime.sendMessage(msg, cb); } catch (e) { /* context gone */ }
  }

  function nameFor(url) {
    try {
      const u = new URL(url);
      return decodeURIComponent((u.pathname.split("/").filter(Boolean).pop() || u.hostname));
    } catch (e) { return String(url).slice(0, 48); }
  }

  function kindOf(m) {
    if (/\.m3u8(\?|#|$)/i.test(m.url) || m.ctype === "hls") return "HLS";
    if (/\.mpd(\?|#|$)/i.test(m.url) || m.ctype === "dash") return "DASH";
    return (m.type || "FILE").toUpperCase();
  }
  function isStream(m) { const k = kindOf(m); return k === "HLS" || k === "DASH"; }

  // Media found directly in the DOM (real http(s) sources, not blob:/data:).
  function domMedia() {
    const out = [];
    document.querySelectorAll("video, audio").forEach((v) => {
      const srcs = new Set();
      if (v.currentSrc) srcs.add(v.currentSrc);
      if (v.getAttribute && v.getAttribute("src")) srcs.add(v.src);
      v.querySelectorAll("source").forEach((s) => { if (s.src) srcs.add(s.src); });
      srcs.forEach((u) => {
        if (/^https?:/i.test(u)) out.push({ url: u, type: v.tagName.toLowerCase() === "audio" ? "audio" : "video" });
      });
    });
    return out;
  }

  function merge(sniffed) {
    const map = new Map();
    [...domMedia(), ...(sniffed || [])].forEach((m) => { if (!map.has(m.url)) map.set(m.url, m); });
    return [...map.values()];
  }

  const CSS = `
    :host { all: initial; }
    .fab { font: 600 13px system-ui, sans-serif; cursor: pointer; border: none;
      background: #1769ff; color: #fff; border-radius: 20px; padding: 8px 12px;
      box-shadow: 0 2px 10px rgba(0,0,0,.3); }
    .fab:hover { background: #0f55db; }
    .panel { margin-top: 8px; width: 320px; max-height: 320px; overflow: auto;
      background: #1e1e1e; color: #eee; border-radius: 10px; padding: 8px;
      box-shadow: 0 6px 24px rgba(0,0,0,.4); font: 13px system-ui, sans-serif; }
    .head { display: flex; justify-content: space-between; align-items: center;
      font-weight: 600; padding: 2px 4px 8px; }
    .x { background: transparent; border: none; color: #aaa; font-size: 16px; cursor: pointer; }
    .row { display: flex; align-items: center; gap: 8px; padding: 6px 4px;
      border-top: 1px solid #333; }
    .badge { flex: 0 0 auto; font-size: 10px; font-weight: 700; background: #333;
      color: #9cd; border-radius: 4px; padding: 2px 5px; }
    .name { flex: 1 1 auto; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .dl { flex: 0 0 auto; cursor: pointer; border: none; background: #2a7; color: #fff;
      border-radius: 6px; padding: 5px 9px; font: inherit; }
    .dl:disabled { background: #444; color: #999; cursor: default; }
    .empty { opacity: .6; padding: 8px 4px; }
  `;

  function ensureUI() {
    if (host) return;
    host = document.createElement("div");
    host.style.cssText = "position:fixed;z-index:2147483647;top:14px;right:14px;text-align:right;";
    shadow = host.attachShadow({ mode: "open" });
    const style = document.createElement("style");
    style.textContent = CSS;
    fab = document.createElement("button");
    fab.className = "fab";
    fab.addEventListener("click", () => { collapsed = !collapsed; draw(); });
    panel = document.createElement("div");
    panel.className = "panel";
    const head = document.createElement("div");
    head.className = "head";
    head.append(document.createTextNode("Download media"));
    const x = document.createElement("button");
    x.className = "x"; x.textContent = "×"; x.title = "Hide on this page";
    x.addEventListener("click", () => { hiddenForPage = true; draw(); });
    head.append(x);
    listEl = document.createElement("div");
    panel.append(head, listEl);
    shadow.append(style, fab, panel);
    (document.documentElement || document.body).append(host);
  }

  function draw() {
    if (!enabled || hiddenForPage || items.length === 0) {
      if (host) host.style.display = "none";
      return;
    }
    ensureUI();
    host.style.display = "";
    fab.textContent = "⬇ " + items.length + (items.length === 1 ? " media" : " media");
    panel.style.display = collapsed ? "none" : "block";
    listEl.textContent = "";
    items.forEach((m) => {
      const row = document.createElement("div");
      row.className = "row";
      const badge = document.createElement("span");
      badge.className = "badge"; badge.textContent = kindOf(m);
      const name = document.createElement("span");
      name.className = "name"; name.textContent = nameFor(m.url); name.title = m.url;
      const dl = document.createElement("button");
      dl.className = "dl"; dl.textContent = "Download";
      dl.addEventListener("click", () => {
        send({ type: "downloadMedia", url: m.url, filename: nameFor(m.url),
               pageUrl: location.href, title: document.title,
               stream: isStream(m), ctype: m.ctype });
        dl.textContent = "Sent ✓"; dl.disabled = true;
      });
      row.append(badge, name, dl);
      listEl.append(row);
    });
  }

  function refresh() {
    if (!enabled) { if (host) host.style.display = "none"; return; }
    send({ type: "getMedia" }, (resp) => {
      items = merge(resp && resp.media);
      draw();
    });
  }

  chrome.storage.local.get({ videoPanel: true }, (d) => {
    enabled = d.videoPanel !== false;
    refresh();
  });
  chrome.storage.onChanged.addListener((ch, area) => {
    if (area === "local" && ch.videoPanel) {
      enabled = ch.videoPanel.newValue !== false;
      if (!enabled && host) host.style.display = "none"; else refresh();
    }
  });

  let t = null;
  const mo = new MutationObserver(() => { clearTimeout(t); t = setTimeout(refresh, 800); });
  try { mo.observe(document.documentElement, { childList: true, subtree: true }); } catch (e) { /* */ }
  setInterval(refresh, 2500);
  refresh();
})();
