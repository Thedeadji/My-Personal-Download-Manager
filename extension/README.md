# MyPersonalDownloadManager — Browser Integration

This folder is an unpacked **MV3 browser extension** (Chrome/Edge; Firefox works
with minor manifest tweaks) that sends downloads to the MPDM engine through a
**native messaging host**.

```
  Browser extension                 Native host (Python)            MPDM engine
 ┌──────────────────┐   stdin/stdout  ┌────────────────────┐        ┌──────────┐
 │ context menu /   │  4-byte len +   │ mpdm/native_host.py │  calls │ engine.py│
 │ download capture │────JSON────────►│ read_message()     │───────►│ Download │
 │ (url+cookies+ref)│◄───JSON─────────│ write_message()    │◄───────│ progress │
 └──────────────────┘   progress/done └────────────────────┘        └──────────┘
```

## Why this design

This is exactly how a mainstream download manager integrates with browsers —
a lightweight extension plus a native-messaging host — **without** any kernel
driver or process-memory hooking. The extension collects the URL, a suggested
filename, the page `Referer`, and the site **cookies** (via `chrome.cookies`)
so downloads behind a login replay correctly, then passes them to the host.

## Install (Chrome / Edge / Brave / Vivaldi)

1. **Install MPDM** (the Windows installer, or the Linux `.deb`). It ships this
   folder and the native host `mpdm_host`.

2. **Load the extension**
   - Open `chrome://extensions` (or `edge://extensions`, `brave://extensions`).
   - Turn on **Developer mode**.
   - Click **Load unpacked** and select this `extension` folder.
   - Copy the extension's **ID** shown on its card.

   **Connect it**: in MPDM open **Settings → Browser → Connected browsers**, paste
   the ID and click **Add**. Or from a terminal: `mpdm_host --register --extension-id <ID>`.
   Repeat for every browser you use; each one is added to the list.

3. **Verify** — click the extension icon → **Test connection to host**. You
   should see “Host connected ✓”.

## Use

- **Right-click** any link, image, video, or audio → **Download with
  MyPersonalDownloadManager**.
- Or open the popup and enable **Capture all browser downloads** to route every
  browser download to MPDM automatically (off by default so nothing is hijacked
  without your consent).

Completed files land in your Downloads folder (or wherever `MDM_OUTPUT_DIR` /
Settings point). Progress and completion show as desktop notifications.

## Uninstall

Remove the browser in **Settings → Browser → Connected browsers** (or run
`mpdm_host --unregister`, which removes all of them) and remove the extension from
the browser. Uninstalling MPDM on Windows unregisters everything automatically.

## Notes / limits

- The host runs `python -m mpdm.native_host`; Python must be on PATH (the
  installer bakes the current interpreter path into a launcher shim).
- `sendNativeMessage` returns only the first reply; live progress uses a
  long-lived `connectNative` port (see `background.js`).
- Firefox uses `allowed_extensions` + a different manifest location; the
  installer handles both, but you'll want an `applications.gecko.id` in the
  manifest for a real Firefox build.


## Firefox

The same folder works in Firefox 121+ (the manifest carries both a Chrome
`service_worker` and a Firefox background `scripts` entry, plus the fixed add-on ID
`mpdm@mypersonaldownloadmanager`).

1. In MPDM: **Settings → Browser → Connect Firefox** (the Windows installer does this
   for you).
2. In Firefox: `about:debugging` → **This Firefox** → **Load Temporary Add-on…** →
   pick this folder's `manifest.json`.

Release Firefox only runs *unsigned* add-ons temporarily (until Firefox restarts). To
keep it installed, either sign it for free as an unlisted add-on on
addons.mozilla.org, or use Firefox Developer Edition / Nightly with
`xpinstall.signatures.required` set to `false`.
