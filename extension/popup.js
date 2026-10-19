// Popup controls: toggle download interception and test the native host.

const cb = document.getElementById("intercept");
const confirmCb = document.getElementById("confirm");
const statusEl = document.getElementById("status");
const pingBtn = document.getElementById("ping");

chrome.runtime.sendMessage({ type: "getIntercept" }, (d) => {
  if (d) cb.checked = !!d.intercept;
});
chrome.runtime.sendMessage({ type: "getConfirm" }, (d) => {
  if (d) confirmCb.checked = !!d.confirm;
});

confirmCb.addEventListener("change", () => {
  chrome.runtime.sendMessage({ type: "setConfirm", value: confirmCb.checked }, () => {
    statusEl.textContent = confirmCb.checked
      ? "You'll get a Download / Cancel prompt before each download."
      : "Downloads start immediately, no prompt.";
  });
});

cb.addEventListener("change", () => {
  chrome.runtime.sendMessage({ type: "setIntercept", value: cb.checked }, () => {
    statusEl.textContent = cb.checked
      ? "All browser downloads will be routed to MPDM."
      : "Only right-click downloads are sent to MPDM.";
  });
});

pingBtn.addEventListener("click", () => {
  statusEl.textContent = "Pinging host…";
  chrome.runtime.sendMessage({ type: "ping" }, (r) => {
    if (r && r.ok && r.resp && r.resp.type === "pong") {
      statusEl.innerHTML = '<span class="ok">Host connected ✓</span>';
    } else {
      statusEl.innerHTML =
        '<span class="bad">Host not reachable.</span> Run install_host.py, then reload.';
    }
  });
});
