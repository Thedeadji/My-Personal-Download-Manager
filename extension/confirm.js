// Confirmation window: shows the pending download and lets the user edit the
// filename / connections, then Download or Cancel.

const params = new URLSearchParams(location.search);
const id = params.get("id");

const fnEl = document.getElementById("filename");
const urlEl = document.getElementById("url");
const connEl = document.getElementById("connections");
const dlBtn = document.getElementById("download");
const cancelBtn = document.getElementById("cancel");

let job = null;

// A placeholder name (no real extension, or the generic "download" Drive gives)
// is left blank so the engine can name the file from the server response.
function isGeneric(name) {
  if (!name) return true;
  const n = name.trim().toLowerCase();
  return n === "download" || !n.includes(".");
}

chrome.storage.local.get({ pending: {} }, (d) => {
  job = (d.pending || {})[id];
  if (!job) {
    urlEl.textContent = "This download request has expired.";
    dlBtn.disabled = true;
    return;
  }
  fnEl.value = isGeneric(job.filename) ? "" : job.filename;
  fnEl.placeholder = "Automatic (named from the server)";
  urlEl.textContent = job.url || "";
  if (job.connections) connEl.value = job.connections;
  fnEl.focus();
  fnEl.select();
});

function finish(start) {
  // Remove the pending entry either way.
  chrome.storage.local.get({ pending: {} }, (d) => {
    const pending = d.pending || {};
    delete pending[id];
    chrome.storage.local.set({ pending }, () => {
      if (start && job) {
        // Empty (or still-generic) -> let the engine name it from the server.
        const typed = fnEl.value.trim();
        job.filename = isGeneric(typed) ? "" : typed;
        job.connections = Math.max(1, Math.min(32, parseInt(connEl.value, 10) || 8));
        chrome.runtime.sendMessage({ type: "startDownload", job }, () => window.close());
      } else {
        window.close();
      }
    });
  });
}

dlBtn.addEventListener("click", () => finish(true));
cancelBtn.addEventListener("click", () => finish(false));
document.addEventListener("keydown", (e) => {
  if (e.key === "Enter") finish(true);
  if (e.key === "Escape") finish(false);
});
