#!/usr/bin/env python3
"""codesign wrapper that keeps parentheses in helper paths intact."""

import subprocess
import sys


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit("usage: codesign-id.py IDENTITY PATH [ENTITLEMENTS]")
    identity, target = sys.argv[1], sys.argv[2]
    entitlements = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
    cmd = [
        "/usr/bin/codesign",
        "--force",
        "--sign",
        identity,
        "--options",
        "runtime",
        "--timestamp=none",
    ]
    if entitlements:
        cmd.extend(["--entitlements", entitlements])
    cmd.append(target)
    subprocess.check_call(cmd)


if __name__ == "__main__":
    main()
