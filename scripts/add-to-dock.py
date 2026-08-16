#!/usr/bin/env python3
"""Add an .app bundle to the macOS Dock if it is not already there."""

from __future__ import annotations

import plistlib
import subprocess
import sys
from pathlib import Path


def dock_plist() -> dict:
    raw = subprocess.check_output(["/usr/bin/defaults", "export", "com.apple.dock", "-"])
    return plistlib.loads(raw)


def already_present(apps: list, uri: str) -> bool:
    needle = uri.rstrip("/")
    for tile in apps:
        data = tile.get("tile-data") or {}
        file_data = data.get("file-data") or {}
        existing = str(file_data.get("_CFURLString") or "").rstrip("/")
        if existing == needle:
            return True
        # Some tiles store a POSIX path instead of a file URL.
        if existing == str(Path(uri.replace("file://", "")).expanduser()):
            return True
    return False


def add(app_path: Path) -> str:
    app_path = app_path.resolve()
    if not app_path.is_dir() or app_path.suffix != ".app":
        raise SystemExit(f"not an app bundle: {app_path}")

    uri = app_path.as_uri()
    plist = dock_plist()
    apps = list(plist.get("persistent-apps") or [])
    if already_present(apps, uri):
        return "already"

    apps.append(
        {
            "tile-type": "file-tile",
            "tile-data": {
                "file-type": 1,
                "file-label": app_path.stem,
                "bundle-identifier": "",
                "file-data": {
                    "_CFURLString": uri,
                    "_CFURLStringType": 15,
                },
            },
        }
    )
    plist["persistent-apps"] = apps

    tmp = Path("/tmp/discord-drover-dock.plist")
    tmp.write_bytes(plistlib.dumps(plist, fmt=plistlib.FMT_BINARY))
    subprocess.check_call(["/usr/bin/defaults", "import", "com.apple.dock", str(tmp)])
    tmp.unlink(missing_ok=True)
    subprocess.check_call(["/usr/bin/killall", "Dock"])
    return "added"


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} /path/to/App.app")
    print(add(Path(sys.argv[1])))


if __name__ == "__main__":
    main()
