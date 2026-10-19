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

## Install (Chrome / Edge)

1. **Load the extension**
   - Open `chrome://extensions` (or `edge://extensions`).
   - Turn on **Developer mode**.
   - Click **Load unpacked** and select this `extension` folder.
   - Copy the extension's **ID** shown on its card.

2. **Install the native host** (registers it so the browser can launch it)
   ```bash
   # from the project root (one level up from this folder)
   python install_host.py --extension-id <THE_ID_YOU_COPIED> --browser chrome
   ```
   Use `--browser edge` for Edge, or `--browser all` for both. On Windows this
   also writes the required registry key under
   `HKCU\Software\Google\Chrome\NativeMessagingHosts`.

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

```bash
python install_host.py --uninstall --browser all
```
and remove the extension from `chrome://extensions`.

## Notes / limits

- The host runs `python -m mpdm.native_host`; Python must be on PATH (the
  installer bakes the current interpreter path into a launcher shim).
- `sendNativeMessage` returns only the first reply; live progress uses a
  long-lived `connectNative` port (see `background.js`).
- Firefox uses `allowed_extensions` + a different manifest location; the
  installer handles both, but you'll want an `applications.gecko.id` in the
  manifest for a real Firefox build.
