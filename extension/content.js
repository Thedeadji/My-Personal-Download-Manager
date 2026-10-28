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

  // Industrial theme (matches the app/web): chamfered panels, amber accent,
  // cyan badges. Isolated in the shadow root, so these are the only styles.
  const CSS = `
    :host { all: initial; }
    .fab { font: 600 12px/1 system-ui, sans-serif; letter-spacing: .04em;
      cursor: pointer; border: 1px solid #ffa62b;
      background: #ffa62b; color: #1c1203; padding: 9px 13px;
      clip-path: polygon(7px 0,100% 0,100% calc(100% - 7px),calc(100% - 7px) 100%,0 100%,0 7px);
      box-shadow: 0 2px 10px rgba(0,0,0,.35); }
    .fab:hover { filter: brightness(1.08); }
    .panel { margin-top: 8px; width: 320px; max-height: 320px; overflow: auto;
      background: #222b35; color: #e6ebf0; padding: 10px;
      border: 1px solid #2e3946;
      clip-path: polygon(10px 0,100% 0,100% calc(100% - 10px),calc(100% - 10px) 100%,0 100%,0 10px);
      box-shadow: 0 6px 24px rgba(0,0,0,.45); font: 13px system-ui, sans-serif; }
    .head { display: flex; justify-content: space-between; align-items: center;
      font: 600 11px/1 system-ui, sans-serif; letter-spacing: .10em; text-transform: uppercase;
      color: #8b97a5; padding: 2px 2px 10px; }
    .x { background: transparent; border: none; color: #8b97a5; font-size: 16px; cursor: pointer; line-height: 1; }
    .x:hover { color: #ffa62b; }
    .row { display: flex; align-items: center; gap: 8px; padding: 7px 2px;
      border-top: 1px solid rgba(255,255,255,.06); }
    .badge { flex: 0 0 auto; font: 700 10px/1 "JetBrains Mono", ui-monospace, monospace;
      letter-spacing: .04em; background: #0c1116; color: #3be0ff;
      border: 1px solid #0e2a33; padding: 3px 5px; }
    .name { flex: 1 1 auto; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #e6ebf0; }
    .dl { flex: 0 0 auto; cursor: pointer; border: 1px solid #ffa62b;
      background: #ffa62b; color: #1c1203; padding: 6px 10px;
      font: 600 11px/1 system-ui, sans-serif; letter-spacing: .04em;
      clip-path: polygon(5px 0,100% 0,100% calc(100% - 5px),calc(100% - 5px) 100%,0 100%,0 5px); }
    .dl:hover { filter: brightness(1.08); }
    .dl:disabled { background: #0c1116; color: #6bd66f; border-color: #0e2a33; cursor: default; filter: none; }
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
    // SPA sites (YouTube, etc.) can detach our host during re-renders — reattach.
    if (host && !host.isConnected) (document.documentElement || document.body).append(host);
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

  // Handle SPA navigation (History API): the content script keeps running but
  // the "page" changed, so reset and re-scan a few times to catch new media.
  let lastHref = location.href;
  function onNav() {
    hiddenForPage = false;
    collapsed = true;
    items = [];
    if (host) host.style.display = "none";
    send({ type: "clearMedia" });               // drop the previous page's sniffed media
    [500, 1500, 3000].forEach((ms) => setTimeout(refresh, ms));
  }
  setInterval(() => {
    if (location.href !== lastHref) { lastHref = location.href; onNav(); }
  }, 700);

  let t = null;
  const mo = new MutationObserver(() => { clearTimeout(t); t = setTimeout(refresh, 800); });
  try { mo.observe(document.documentElement, { childList: true, subtree: true }); } catch (e) { /* */ }
  setInterval(refresh, 2500);
  refresh();
})();
