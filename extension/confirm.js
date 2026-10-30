// "Add downloads" window: shows the download(s) the extension captured and lets
// the user edit the links (with [1-100]/[001-120]/[a-e] range expansion), pick a
// folder, connections, speed limit and proxy, then add them to the MPDM queue.

const params = new URLSearchParams(location.search);
const id = params.get("id");

const linksEl = document.getElementById("links");
const previewEl = document.getElementById("preview");
const previewLbl = document.getElementById("previewLbl");
const previewList = document.getElementById("previewList");
const showAllBtn = document.getElementById("showAll");
const savetoEl = document.getElementById("saveto");
const connEl = document.getElementById("connections");
const speedEl = document.getElementById("speed");
const renameEl = document.getElementById("rename");
const incognitoEl = document.getElementById("incognito");
const browserUaEl = document.getElementById("browserua");
const dlBtn = document.getElementById("download");
const addLabel = document.getElementById("addLabel");
const cancelBtn = document.getElementById("cancel");
const browseBtn = document.getElementById("browse");

// A real confirm is actioned within seconds. Anything older is a window the
// browser restored from a previous session — close it rather than re-prompting.
const STALE_MS = 2 * 60 * 1000;
const MAX_EXPAND = 10000;

let job = null;
let showAll = false;
let lastExpanded = [];
// Once the user edits the Rename box we stop auto-filling it from the URL.
let renameTouched = false;

// ---- range expansion (mirrors batch.py expand_pattern/expand_urls) ----------

// A [a-b] spec -> its sequence of substitutions, or null if it isn't a range
// (then the literal "[spec]" is kept as-is).
function rangeSeq(spec) {
  const num = spec.match(/^(\d+)-(\d+)$/);
  if (num) {
    const a = parseInt(num[1], 10), b = parseInt(num[2], 10);
    const width = (num[1].length === num[2].length && num[1][0] === "0") ? num[1].length : 0;
    const seq = [], step = a <= b ? 1 : -1;
    for (let i = a; step > 0 ? i <= b : i >= b; i += step)
      seq.push(width ? String(i).padStart(width, "0") : String(i));
    return seq;
  }
  const alpha = spec.match(/^([a-zA-Z])-([a-zA-Z])$/);
  if (alpha) {
    const a = alpha[1].charCodeAt(0), b = alpha[2].charCodeAt(0);
    const seq = [], step = a <= b ? 1 : -1;
    for (let i = a; step > 0 ? i <= b : i >= b; i += step) seq.push(String.fromCharCode(i));
    return seq;
  }
  return null;
}

// Expand one line's [a-b] ranges into a cartesian product of concrete URLs.
function expandPattern(line) {
  // Tokenize into fixed literals and variable sequences.
  const tokens = []; // string (literal) | string[] (choices)
  let cursor = 0;
  const re = /\[([^\]]+)\]/g;
  let m, hasRange = false;
  while ((m = re.exec(line)) !== null) {
    if (m.index > cursor) tokens.push(line.slice(cursor, m.index));
    const seq = rangeSeq(m[1]);
    if (seq) { tokens.push(seq); hasRange = true; }
    else tokens.push(m[0]); // keep "[spec]" literally
    cursor = m.index + m[0].length;
  }
  if (cursor < line.length) tokens.push(line.slice(cursor));
  if (!hasRange) return [line];

  let results = [""];
  for (const tok of tokens) {
    if (typeof tok === "string") {
      results = results.map((r) => r + tok);
    } else {
      const next = [];
      for (const r of results)
        for (const s of tok) {
          next.push(r + s);
          if (next.length >= MAX_EXPAND) break;
        }
      results = next;
    }
    if (results.length >= MAX_EXPAND) break;
  }
  return results.slice(0, MAX_EXPAND);
}

function expandAll() {
  const lines = linksEl.value.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  const out = [];
  const seen = new Set();
  for (const line of lines) {
    for (const u of expandPattern(line)) {
      if (!seen.has(u)) { seen.add(u); out.push(u); }
      if (out.length >= MAX_EXPAND) break;
    }
    if (out.length >= MAX_EXPAND) break;
  }
  return out;
}

function fileNameOf(url) {
  try {
    const u = new URL(url);
    return decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || url);
  } catch (e) {
    return url.split("/").filter(Boolean).pop() || url;
  }
}

// A name worth pre-filling into Rename: a real filename (has an extension) that
// isn't a generic endpoint token. Otherwise "", so we leave the box blank and let
// the host use the server's filename (the real downloaded name).
function cleanName(url) {
  const seg = fileNameOf(url).split(/[?#]/)[0];
  if (!seg || !/\.[A-Za-z0-9]{1,8}$/.test(seg)) return "";
  if (/^(download|uc|open|view|file|get|dl|export)$/i.test(seg.replace(/\.[^.]*$/, ""))) return "";
  return seg;
}

// ---- preview + label --------------------------------------------------------

function refresh() {
  const urls = expandAll();
  lastExpanded = urls;
  const n = urls.length;

  // Rename applies to a single download; keep the box showing that file's name
  // until the user types their own. When the URL carries no real filename (e.g.
  // a Google Drive "…/download?id=" link), leave it blank so the server's name is
  // used instead of a junk "download". A batch is auto-named per file.
  if (n === 1) {
    renameEl.disabled = false;
    renameEl.placeholder = "Uses the server's filename";
    if (!renameTouched) renameEl.value = cleanName(urls[0]);
  } else {
    renameEl.disabled = true;
    renameEl.placeholder = "Auto-named per file";
  }

  if (n <= 1) {
    previewEl.classList.remove("show");
  } else {
    previewEl.classList.add("show");
    previewLbl.innerHTML = `Expands to <b>${n}</b> links`;
    const names = urls.map(fileNameOf);
    previewList.innerHTML = "";
    const shown = showAll ? names : names.slice(0, 3);
    for (const nm of shown) {
      const li = document.createElement("li");
      li.textContent = nm;
      previewList.appendChild(li);
    }
    if (!showAll && n > 3) {
      const li = document.createElement("li");
      li.className = "more";
      li.textContent = `…${n - 3} more, up to ${names[names.length - 1]}`;
      previewList.appendChild(li);
    }
    showAllBtn.textContent = showAll ? "Show less" : "Show all";
  }

  addLabel.textContent = n > 1 ? `Add ${n} downloads` : "Add download";
  dlBtn.disabled = n === 0;
  dlBtn.style.opacity = n === 0 ? "0.5" : "1";
}

// ---- speed parsing ("500 KB/s", "2 MB", "unlimited") -> bytes/s -------------

function parseRate(text) {
  const s = (text || "").trim().toLowerCase();
  if (!s || s === "unlimited" || s === "0") return 0;
  const m = s.match(/^([\d.]+)\s*([kmg]?)b?(?:\/s|ps)?$/);
  if (!m) return 0;
  const n = parseFloat(m[1]);
  if (!isFinite(n)) return 0;
  const mult = { "": 1, k: 1024, m: 1024 * 1024, g: 1024 * 1024 * 1024 }[m[2]] || 1;
  return Math.round(n * mult);
}

// ---- load the pending job ---------------------------------------------------

chrome.storage.local.get({ pending: {} }, (d) => {
  job = (d.pending || {})[id];
  // Missing (cleared on browser startup) or stale (restored window) -> this is
  // not a live request. Close silently instead of re-prompting the user.
  if (!job || (job.ts && Date.now() - job.ts > STALE_MS)) {
    window.close();
    return;
  }
  linksEl.value = job.url || "";
  if (job.connections) connEl.value = String(job.connections);
  if (job.incognito) incognitoEl.checked = true;
  // Pre-fill the Rename box with the file's name; a name the host already resolved
  // wins and counts as user-set so refresh() won't overwrite it. A generic/missing
  // name stays blank so the server's filename is used (not "download").
  if (job.filename) { renameEl.value = job.filename; renameTouched = true; }
  else renameEl.value = cleanName(job.url || "");
  refresh();
  linksEl.focus();
});

renameEl.addEventListener("input", () => { renameTouched = true; });
linksEl.addEventListener("input", () => { showAll = false; refresh(); });
showAllBtn.addEventListener("click", () => { showAll = !showAll; refresh(); });

// ---- finish -----------------------------------------------------------------

function finish(start) {
  chrome.storage.local.get({ pending: {} }, (d) => {
    const pending = d.pending || {};
    delete pending[id];
    chrome.storage.local.set({ pending }, () => {
      if (!start || !job) { window.close(); return; }
      const urls = lastExpanded.length ? lastExpanded : expandAll();
      const conns = Math.max(1, Math.min(32, parseInt(connEl.value, 10) || 8));
      const rate = parseRate(speedEl.value);
      const outDir = savetoEl.value.trim();
      const renameVal = renameEl.value.trim();
      const incognito = incognitoEl.checked;
      const honestUA = !browserUaEl.checked;
      const single = urls.length === 1;

      for (const url of urls) {
        const j = {
          url,
          // One URL uses the (possibly renamed) name; blank -> the host uses the
          // server's real filename. A batch is auto-named per file.
          filename: single ? renameVal : "",
          connections: conns,
          headers: Object.assign({}, job.headers || {}),
          stream: !!job.stream,
          incognito,
          honestUA,
        };
        if (rate) j.rate = rate;
        if (outDir) j.output_dir = outDir;
        delete j.id; // the host assigns a fresh id per job
        chrome.runtime.sendMessage({ type: "startDownload", job: j });
      }
      window.close();
    });
  });
}

dlBtn.addEventListener("click", () => finish(true));
cancelBtn.addEventListener("click", () => finish(false));

// "Save as…": ask the native host to open the system Save-As dialog (the browser
// popup can't open one itself), then split the chosen path into folder + name.
browseBtn.addEventListener("click", () => {
  const first = expandAll()[0] || linksEl.value.trim();
  const suggested = renameEl.value.trim() || cleanName(first) || "download";
  chrome.runtime.sendMessage({ type: "browseSaveAs", filename: suggested }, (resp) => {
    const p = resp && resp.path;
    if (!p) return;
    const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"));
    const dir = i >= 0 ? p.slice(0, i) : "";
    const file = i >= 0 ? p.slice(i + 1) : p;
    if (dir) savetoEl.value = dir;
    if (file) { renameEl.value = file; renameTouched = true; }
  });
});

document.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) finish(true);
  if (e.key === "Escape") finish(false);
});
