// MyPersonalDownloadManager — browser integration (MV3 service worker)
//
// Downloads reach MPDM via:
//   1. Right-click a link / page / media  ->  "Download with MyPersonalDownloadManager".
//   2. (Optional) "Capture all browser downloads" — intercept normal downloads.
//
// Before anything is sent to the engine, an optional confirmation window shows
// the filename + URL with Download / Cancel (and lets you tweak the filename and
// connection count). It's on by default. Each confirmed job is dispatched
// exactly once over a single native-messaging port; a set of in-flight URLs
// blocks accidental duplicates.

const HOST_NAME = "com.mypersonaldownloadmanager.host";
const MENU_ROOT = "mpdm-download";

const inFlight = new Set(); // URLs currently downloading

// ---- helpers -------------------------------------------------------------

async function getCookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    if (!cookies || cookies.length === 0) return "";
    return cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  } catch (e) {
    return "";
  }
}

function filenameFromUrl(url) {
  try {
    const u = new URL(url);
    return decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || "");
  } catch (e) {
    return "";
  }
}

function notify(title, message) {
  try {
    chrome.notifications.create({
      type: "basic",
      iconUrl: "icons/icon48.png",
      title,
      message: String(message).slice(0, 300),
    });
  } catch (e) {
    /* optional */
  }
}

// ---- media detection for the on-page "download video" panel --------------
// We passively watch network requests per tab and remember ones that look like
// downloadable media (direct files and HLS/DASH manifests). The content script
// asks for this list to build its floating panel.

const MEDIA_RE = /\.(m3u8|mpd|mp4|m4v|webm|mkv|mov|flv|mp3|m4a|aac|flac|ogg|wav)(\?|#|$)/i;
const AUDIO_RE = /\.(mp3|m4a|aac|flac|ogg|wav)(\?|#|$)/i;
const mediaByTab = new Map(); // tabId -> Map(url -> {url,type,ctype,size})

function isStreamUrl(url, ctype) {
  return /\.(m3u8|mpd)(\?|#|$)/i.test(url) || ctype === "hls" || ctype === "dash";
}

function addMedia(tabId, url, info) {
  if (tabId < 0 || !url || !/^https?:/i.test(url)) return;
  let m = mediaByTab.get(tabId);
  if (!m) { m = new Map(); mediaByTab.set(tabId, m); }
  if (!m.has(url) && m.size > 60) return; // cap per tab
  m.set(url, Object.assign(m.get(url) || { url }, info));
}

function ctypeKind(ct) {
  ct = (ct || "").toLowerCase();
  if (ct.includes("mpegurl")) return { type: "video", ctype: "hls" };
  if (ct.includes("dash+xml")) return { type: "video", ctype: "dash" };
  if (ct.startsWith("video/")) return { type: "video" };
  if (ct.startsWith("audio/")) return { type: "audio" };
  return null;
}

chrome.webRequest.onBeforeRequest.addListener((d) => {
  if (MEDIA_RE.test(d.url)) {
    addMedia(d.tabId, d.url, { type: AUDIO_RE.test(d.url) ? "audio" : "video" });
  }
}, { urls: ["<all_urls>"] });

chrome.webRequest.onHeadersReceived.addListener((d) => {
  let ct = "", len = 0;
  for (const h of d.responseHeaders || []) {
    const n = h.name.toLowerCase();
    if (n === "content-type") ct = h.value || "";
    else if (n === "content-length") len = parseInt(h.value || "0", 10) || 0;
  }
  const k = ctypeKind(ct);
  if (k) addMedia(d.tabId, d.url, Object.assign({ size: len || undefined }, k));
}, { urls: ["<all_urls>"] }, ["responseHeaders"]);

function clearTabMedia(tabId) { mediaByTab.delete(tabId); }
chrome.tabs.onRemoved.addListener(clearTabMedia);
chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (info.status === "loading" && info.url) clearTabMedia(tabId);
});

// ---- confirmation gate ---------------------------------------------------

async function queueJob(job) {
  job.id = job.id || crypto.randomUUID().slice(0, 12);
  const { confirm } = await chrome.storage.local.get({ confirm: true });
  if (!confirm) {
    sendToHost(job);
    return;
  }
  // Stash the pending job and open a small confirmation window.
  const { pending } = await chrome.storage.local.get({ pending: {} });
  pending[job.id] = job;
  await chrome.storage.local.set({ pending });
  chrome.windows.create({
    url: chrome.runtime.getURL(`confirm.html?id=${job.id}`),
    type: "popup",
    width: 480,
    height: 340,
  });
}

// ---- single-dispatch job runner -----------------------------------------

function runJob(job) {
  if (inFlight.has(job.url)) return;
  inFlight.add(job.url);

  let port;
  try {
    port = chrome.runtime.connectNative(HOST_NAME);
  } catch (e) {
    inFlight.delete(job.url);
    notify("MPDM error", "Could not reach the host. Run install_host.py. " + e);
    return;
  }

  let settled = false;
  const cleanup = () => {
    inFlight.delete(job.url);
    try { port.disconnect(); } catch (e) { /* gone */ }
  };

  port.onMessage.addListener((msg) => {
    if (!msg) return;
    if (msg.type === "accepted") notify("Download started", job.filename || job.url);
    else if (msg.type === "done") { settled = true; notify("Download complete", msg.path || job.url); cleanup(); }
    else if (msg.type === "error") { settled = true; notify("Download failed", msg.message || job.url); cleanup(); }
  });
  port.onDisconnect.addListener(() => {
    const err = chrome.runtime.lastError;
    if (!settled && err) notify("MPDM host not reachable", (err.message || "disconnected") + " — run install_host.py");
    inFlight.delete(job.url);
  });

  try {
    port.postMessage(job);
  } catch (e) {
    cleanup();
    notify("MPDM error", String(e));
  }
}

// Google Drive serves large files from a signed *.googleusercontent.com URL tied
// to the browser session, which the engine can't replay. Rewrite Drive downloads
// back to a canonical uc?id=<FILE_ID> URL (id taken from the URL or the referring
// Drive page) so the engine's resolver can do the proper confirm flow.
function driveFileId(s) {
  if (!s) return null;
  let m = s.match(/\/file\/d\/([A-Za-z0-9_-]{10,})/) ||
          s.match(/[?&]id=([A-Za-z0-9_-]{10,})/) ||
          s.match(/\/d\/([A-Za-z0-9_-]{10,})/);
  return m ? m[1] : null;
}

function canonicalizeDrive(url, referer) {
  let host = "";
  try { host = new URL(url).host; } catch (e) { return null; }
  const isDrive = /(^|\.)drive\.google\.com$/.test(host) ||
                  /(^|\.)drive\.usercontent\.google\.com$/.test(host) ||
                  host.endsWith("googleusercontent.com");
  if (!isDrive) return null;
  const id = driveFileId(url) || driveFileId(referer);
  return id ? "https://drive.google.com/uc?export=download&id=" + id : null;
}

async function sendToHost(job) {
  job.headers = job.headers || {};
  const canon = canonicalizeDrive(job.url, job.headers.Referer);
  if (canon) job.url = canon;
  const cookie = await getCookieHeader(job.url);
  if (cookie) job.headers["Cookie"] = cookie;
  if (!job.headers["User-Agent"]) job.headers["User-Agent"] = navigator.userAgent;
  job.type = "download";
  job.id = job.id || crypto.randomUUID().slice(0, 12);
  runJob(job);
}

// ---- context menu --------------------------------------------------------

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: MENU_ROOT,
    title: "Download with MyPersonalDownloadManager",
    contexts: ["link", "image", "video", "audio", "page", "selection"],
  });
  chrome.storage.local.get({ intercept: false, confirm: true }, () => {});
});

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId !== MENU_ROOT) return;
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (!url) return;
  await queueJob({
    url,
    filename: filenameFromUrl(url),
    headers: { Referer: info.pageUrl || (tab && tab.url) || "" },
  });
});

// ---- optional download interception --------------------------------------

chrome.downloads.onCreated.addListener(async (item) => {
  const { intercept } = await chrome.storage.local.get({ intercept: false });
  if (!intercept) return;
  if (!item.url || item.url.startsWith("blob:") || item.url.startsWith("data:")) return;

  const url = item.finalUrl || item.url;
  if (inFlight.has(url)) return;

  try {
    await chrome.downloads.cancel(item.id);
    await chrome.downloads.erase({ id: item.id });
  } catch (e) {
    /* already done; fall through */
  }
  await queueJob({
    url,
    filename: item.filename ? item.filename.split(/[\\/]/).pop() : filenameFromUrl(url),
    headers: { Referer: item.referrer || "" },
  });
});

// ---- messaging (popup + confirm window) ----------------------------------

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg) return;
  if (msg.type === "startDownload" && msg.job) {
    sendToHost(msg.job);
    sendResponse({ ok: true });
    return true;
  }
  if (msg.type === "setIntercept") {
    chrome.storage.local.set({ intercept: !!msg.value }, () => sendResponse({ ok: true }));
    return true;
  }
  if (msg.type === "getIntercept") {
    chrome.storage.local.get({ intercept: false }, (d) => sendResponse(d));
    return true;
  }
  if (msg.type === "setConfirm") {
    chrome.storage.local.set({ confirm: !!msg.value }, () => sendResponse({ ok: true }));
    return true;
  }
  if (msg.type === "getMedia") {
    const tabId = sender.tab ? sender.tab.id : msg.tabId;
    const m = mediaByTab.get(tabId);
    sendResponse({ media: m ? [...m.values()] : [] });
    return true;
  }
  if (msg.type === "clearMedia") {
    const tabId = sender.tab ? sender.tab.id : msg.tabId;
    if (tabId != null) clearTabMedia(tabId);
    sendResponse({ ok: true });
    return true;
  }
  if (msg.type === "downloadMedia" && msg.url) {
    const stream = !!msg.stream || isStreamUrl(msg.url, msg.ctype);
    let filename = msg.filename || filenameFromUrl(msg.url) || "video";
    if (stream && !/\.(mp4|mkv|webm)$/i.test(filename)) filename += ".mp4";
    queueJob({
      url: msg.url, filename, stream,
      headers: { Referer: msg.pageUrl || "" },
    });
    sendResponse({ ok: true });
    return true;
  }
  if (msg.type === "getVideoPanel") {
    chrome.storage.local.get({ videoPanel: true }, (d) => sendResponse(d));
    return true;
  }
  if (msg.type === "setVideoPanel") {
    chrome.storage.local.set({ videoPanel: !!msg.value }, () => sendResponse({ ok: true }));
    return true;
  }
  if (msg.type === "getConfirm") {
    chrome.storage.local.get({ confirm: true }, (d) => sendResponse(d));
    return true;
  }
  if (msg.type === "ping") {
    chrome.runtime.sendNativeMessage(HOST_NAME, { type: "ping" }, (resp) => {
      sendResponse({ ok: !chrome.runtime.lastError, resp, error: chrome.runtime.lastError?.message });
    });
    return true;
  }
});
