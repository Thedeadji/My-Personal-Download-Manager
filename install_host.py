#!/usr/bin/env python3
"""Register the MyPersonalDownloadManager native messaging host with Chrome/Edge/Firefox.

A native messaging host needs two things:
  1. A manifest JSON that tells the browser how to launch the host and which
     extensions are allowed to talk to it (`allowed_origins`).
  2. On Windows, a registry value pointing the browser at that manifest.
     On macOS/Linux, the manifest just lives in a well-known directory.

Usage:
    python install_host.py --extension-id <ID> [--browser chrome|edge|firefox|all]
    python install_host.py --uninstall [--browser ...]

Find <ID> at chrome://extensions (enable Developer mode, load the `extension`
folder unpacked, then copy the extension's ID). Re-run this script with that ID.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

HOST_NAME = "com.mypersonaldownloadmanager.host"
HERE = Path(__file__).resolve().parent


def host_launch_command() -> list[str]:
    """Command the browser will run to start the host."""
    py = sys.executable or "python"
    # Run the module so imports resolve regardless of CWD.
    return [py, "-m", "mpdm.native_host"]


def write_launcher() -> str:
    """Create a small launcher script so the manifest can point at one path.

    Browsers expect a single executable path, so we wrap `python -m mpdm.native_host`
    in a tiny shim that also sets PYTHONPATH to this project.
    """
    cmd = host_launch_command()
    if os.name == "nt":
        launcher = HERE / "mpdm_host.bat"
        launcher.write_text(
            "@echo off\r\n"
            f'set "PYTHONPATH={HERE}"\r\n'
            f'"{cmd[0]}" -m mpdm.native_host %*\r\n',
            encoding="utf-8",
        )
    else:
        launcher = HERE / "mpdm_host.sh"
        launcher.write_text(
            "#!/bin/sh\n"
            f'export PYTHONPATH="{HERE}"\n'
            f'exec "{cmd[0]}" -m mpdm.native_host "$@"\n',
            encoding="utf-8",
        )
        os.chmod(launcher, 0o755)
    return str(launcher)


def manifest_dict(launcher_path: str, extension_id: str, firefox: bool) -> dict:
    m = {
        "name": HOST_NAME,
        "description": "MyPersonalDownloadManager native messaging host",
        "path": launcher_path,
        "type": "stdio",
    }
    if firefox:
        # Firefox identifies extensions by an ID string, not chrome-extension://
        m["allowed_extensions"] = [extension_id or "mpdm@mypersonaldownloadmanager"]
    else:
        m["allowed_origins"] = [f"chrome-extension://{extension_id}/"]
    return m


# ---- per-platform manifest locations -------------------------------------
def manifest_targets(browser: str) -> list[Path]:
    home = Path.home()
    targets: list[Path] = []
    if sys.platform == "darwin":
        base = home / "Library" / "Application Support"
        mapping = {
            "chrome": base / "Google" / "Chrome" / "NativeMessagingHosts",
            "edge": base / "Microsoft Edge" / "NativeMessagingHosts",
            "firefox": home / "Library" / "Application Support" / "Mozilla" / "NativeMessagingHosts",
        }
    elif os.name == "posix":
        cfg = home / ".config"
        mapping = {
            "chrome": cfg / "google-chrome" / "NativeMessagingHosts",
            "edge": cfg / "microsoft-edge" / "NativeMessagingHosts",
            "firefox": home / ".mozilla" / "native-messaging-hosts",
        }
    else:  # Windows: manifest file location is arbitrary; registry points to it.
        base = HERE / "_host_manifests"
        mapping = {
            "chrome": base,
            "edge": base,
            "firefox": base,
        }
    for b in ([browser] if browser != "all" else ["chrome", "edge", "firefox"]):
        targets.append(mapping[b])
    return targets


def win_registry_install(browser: str, manifest_path: str):
    import winreg
    keys = {
        "chrome": r"Software\Google\Chrome\NativeMessagingHosts",
        "edge": r"Software\Microsoft\Edge\NativeMessagingHosts",
        "firefox": r"Software\Mozilla\NativeMessagingHosts",
    }
    for b in ([browser] if browser != "all" else ["chrome", "edge", "firefox"]):
        path = keys[b] + "\\" + HOST_NAME
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, path) as k:
            winreg.SetValueEx(k, None, 0, winreg.REG_SZ, manifest_path)
        print(f"  registry: HKCU\\{path} -> {manifest_path}")


def win_registry_uninstall(browser: str):
    import winreg
    keys = {
        "chrome": r"Software\Google\Chrome\NativeMessagingHosts",
        "edge": r"Software\Microsoft\Edge\NativeMessagingHosts",
        "firefox": r"Software\Mozilla\NativeMessagingHosts",
    }
    for b in ([browser] if browser != "all" else ["chrome", "edge", "firefox"]):
        path = keys[b] + "\\" + HOST_NAME
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
            print(f"  removed registry key HKCU\\{path}")
        except FileNotFoundError:
            pass


def install(extension_id: str, browser: str):
    launcher = write_launcher()
    print(f"launcher: {launcher}")
    for b in ([browser] if browser != "all" else ["chrome", "edge", "firefox"]):
        for target_dir in manifest_targets(b):
            target_dir.mkdir(parents=True, exist_ok=True)
            manifest_file = target_dir / f"{HOST_NAME}.json"
            m = manifest_dict(launcher, extension_id, firefox=(b == "firefox"))
            manifest_file.write_text(json.dumps(m, indent=2), encoding="utf-8")
            print(f"  manifest: {manifest_file}")
            if os.name == "nt":
                win_registry_install(b, str(manifest_file))
    print("\nInstalled. Reload the extension in your browser, then click "
          "“Test connection to host” in the popup.")


def uninstall(browser: str):
    for b in ([browser] if browser != "all" else ["chrome", "edge", "firefox"]):
        for target_dir in manifest_targets(b):
            manifest_file = target_dir / f"{HOST_NAME}.json"
            if manifest_file.exists():
                manifest_file.unlink()
                print(f"  removed {manifest_file}")
        if os.name == "nt":
            win_registry_uninstall(b)
    print("Uninstalled.")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="Install the MPDM native messaging host.")
    p.add_argument("--extension-id", default="", help="Chrome/Edge extension ID (from chrome://extensions)")
    p.add_argument("--browser", choices=["chrome", "edge", "firefox", "all"], default="all")
    p.add_argument("--uninstall", action="store_true")
    args = p.parse_args(argv)

    if args.uninstall:
        uninstall(args.browser)
        return 0

    if not args.extension_id:
        print("WARNING: no --extension-id given. The host will be installed, but "
              "Chrome/Edge will refuse the connection until you re-run with the "
              "real extension ID from chrome://extensions.\n")
    install(args.extension_id, args.browser)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
